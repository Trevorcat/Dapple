#include "engine/exec/gpu_scheduler.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace dapple::engine::gpu {

using hal::Status;

// ------------------------------------------------------------------------------------------------------ pool ---
GpuPool::~GpuPool() { clear(); }

void GpuPool::clear() {
  for (E& e : all_) d_.destroy(e.im);
  all_.clear();
  pooled_ = 0;
}

std::uint32_t GpuPool::bucket(std::uint32_t v) const {
  const std::uint32_t q = v >= 1024 ? 128 : 32;
  return std::min((v + q - 1) / q * q, d_.caps().maxImage2D);
}

Status GpuPool::acquire(Format f, std::uint32_t w, std::uint32_t h, ImageH* out) {
  for (E& e : all_)
    if (e.free && e.im.fmt == f && e.im.w == w && e.im.h == h) {
      e.free = false;
      *out = e.im;
      return Status::Ok;
    }
  ImageH im;
  const Status s = d_.createImage({f, w, h, 1}, &im);
  if (s != Status::Ok) return s;
  pooled_ += im.bytes;
  all_.push_back({im, 0, false});
  *out = im;
  return Status::Ok;
}

void GpuPool::release(const ImageH& im) {
  for (E& e : all_)
    if (e.im.id == im.id) {
      e.free = true;
      e.lastUse = ++tick_;
      return;
    }
}

std::size_t GpuPool::freeBytes() const {
  std::size_t n = 0;
  for (const E& e : all_)
    if (e.free) n += e.im.bytes;
  return n;
}

void GpuPool::trim(std::size_t keepFree) {
  if (freeBytes() <= keepFree) return;
  d_.waitIdle();  // freed images may still be referenced by the last submitted frame
  std::vector<std::size_t> idx;
  for (std::size_t i = 0; i < all_.size(); ++i)
    if (all_[i].free) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return all_[a].lastUse > all_[b].lastUse; });  // newest first
  std::size_t kept = 0;
  std::vector<bool> drop(all_.size(), false);
  for (std::size_t i : idx) {
    if (kept + all_[i].im.bytes <= keepFree) kept += all_[i].im.bytes;
    else drop[i] = true;
  }
  std::vector<E> keep;
  bool any = false;
  for (std::size_t i = 0; i < all_.size(); ++i) {
    if (drop[i]) {
      pooled_ -= all_[i].im.bytes;
      d_.destroy(all_[i].im);
      any = true;
    } else {
      keep.push_back(all_[i]);
    }
  }
  all_ = std::move(keep);
  if (any) d_.trim();  // descriptor sets that named the dropped views are dead weight now
}

// ----------------------------------------------------------------------------------------------------- graph ---
H GpuGraph::external(const ImageH& im) {
  Res r;
  r.img = im;
  r.hasImg = true;
  res_.push_back(r);
  return H(res_.size() - 1);
}
H GpuGraph::externalBuf(const BufferH& b) {
  Res r;
  r.buf = b;
  r.hasBuf = true;
  res_.push_back(r);
  return H(res_.size() - 1);
}
H GpuGraph::transient(Format f, std::uint32_t w, std::uint32_t h, std::uint32_t aw, std::uint32_t ah) {
  Res r;
  r.pooled = true;
  r.fmt = f;
  r.w = aw >= w ? aw : pool_.bucket(w);
  r.h = ah >= h ? ah : pool_.bucket(h);
  res_.push_back(r);
  return H(res_.size() - 1);
}
void GpuGraph::dispatch(const char* name, PipelineH pl, std::initializer_list<Use> uses, std::uint32_t gx, std::uint32_t gy, const void* params,
                        std::size_t paramBytes, std::uint32_t gz, double weight) {
  Node n;
  n.kind = Node::Dispatch;
  n.name = name;
  n.pl = pl;
  n.uses.assign(uses.begin(), uses.end());
  n.gx = gx;
  n.gy = gy;
  n.gz = gz;
  n.weight = weight;
  n.params.assign(static_cast<const std::uint8_t*>(params), static_cast<const std::uint8_t*>(params) + paramBytes);
  nodes_.push_back(std::move(n));
}
void GpuGraph::copyBufferToImage(const BufferH& src, H dst, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint64_t srcOffset) {
  Node n;
  n.kind = Node::BufToImg;
  n.name = "upload";
  n.buf = src;
  n.b = dst;
  n.w = w;
  n.h = h;
  n.d = d;
  n.off = srcOffset;
  nodes_.push_back(std::move(n));
}
void GpuGraph::copyBufferToBuffer(const BufferH& src, H dst, std::uint64_t bytes, std::uint64_t srcOffset) {
  Node n;
  n.kind = Node::BufToBuf;
  n.name = "upload";
  n.buf = src;
  n.b = dst;
  n.bytes = bytes;
  n.off = srcOffset;
  nodes_.push_back(std::move(n));
}
void GpuGraph::copyImageToBuffer(H src, const BufferH& dst, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
  Node n;
  n.kind = Node::ImgToBuf;
  n.name = "readback";
  n.a = src;
  n.buf = dst;
  n.x = x;
  n.y = y;
  n.w = w;
  n.h = h;
  nodes_.push_back(std::move(n));
}
void GpuGraph::copyImage(H src, H dst, std::uint32_t w, std::uint32_t h) {
  Node n;
  n.kind = Node::ImgToImg;
  n.name = "copy";
  n.a = src;
  n.b = dst;
  n.w = w;
  n.h = h;
  nodes_.push_back(std::move(n));
}

// -------------------------------------------------------------------------------------------------- scheduler ---
namespace {
struct Acc {
  H h;
  bool rd, wr;
  char bind;  // binding kind of a dispatch use (I, S, B) or 'T' for transfers
};
}  // namespace

Status submitGraph(GpuGraph& g, hal::CommandList& cl, std::string& err) {
  const auto& nodes = g.nodes_;
  auto& res = g.res_;
  hal::Device& dev = g.dev_;
  struct Guard {  // transient images still held after an error go back to the pool
    GpuGraph& g;
    bool ok = false;
    ~Guard() {
      if (ok) return;
      for (auto& r : g.res_)
        if (r.pooled && r.hasImg) {
          g.pool_.release(r.img);
          r.hasImg = false;
        }
    }
  } guard{g};

  auto accesses = [&](const GpuGraph::Node& n) {
    std::vector<Acc> acc;
    if (n.kind == GpuGraph::Node::Dispatch) {
      for (std::size_t i = 0; i < n.uses.size(); ++i) {
        const char b = n.pl.bindings && i < std::strlen(n.pl.bindings) ? n.pl.bindings[i] : 'I';
        acc.push_back({n.uses[i].res, n.uses[i].read, n.uses[i].write, b});
      }
    } else if (n.kind == GpuGraph::Node::ImgToBuf) {
      acc.push_back({n.a, true, false, 'T'});
    } else if (n.kind == GpuGraph::Node::ImgToImg) {
      acc.push_back({n.a, true, false, 'T'});
      acc.push_back({n.b, false, true, 'T'});
    } else {
      acc.push_back({n.b, false, true, 'T'});
    }
    return acc;
  };
  auto keyOf = [&](H h) -> std::uint64_t {
    const auto& r = res[std::size_t(h)];
    return r.hasImg ? r.img.id : r.hasBuf ? r.buf.id : (1ull << 62) | std::uint64_t(h);
  };

  // Dependency levels: the recorded order is a valid one; passes of independent chains (texture / sharpen / clarity
  // analysis, ...) are regrouped so each level needs one barrier instead of one per pass. Dependencies are
  // read-after-write, write-after-read and write-after-write on the same resource.
  std::vector<int> order(nodes.size());
  {
    std::unordered_map<std::uint64_t, int> lastWriter;
    std::unordered_map<std::uint64_t, std::vector<int>> readers;
    std::vector<int> level(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      int lv = 0;
      const auto acc = accesses(nodes[i]);
      for (const Acc& a : acc) {
        const std::uint64_t k = keyOf(a.h);
        auto w = lastWriter.find(k);
        if (w != lastWriter.end() && w->second != int(i)) lv = std::max(lv, level[std::size_t(w->second)] + 1);
        if (a.wr)
          for (int r : readers[k])
            if (r != int(i)) lv = std::max(lv, level[std::size_t(r)] + 1);
      }
      level[i] = lv;
      for (const Acc& a : acc) {
        const std::uint64_t k = keyOf(a.h);
        if (a.wr) {
          lastWriter[k] = int(i);
          readers[k].clear();
        }
        if (a.rd) readers[k].push_back(int(i));
      }
    }
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return level[std::size_t(a)] < level[std::size_t(b)]; });
  }

  for (auto& r : res) r.last = -1;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[std::size_t(order[i])];
    for (const auto& u : n.uses) res[std::size_t(u.res)].last = int(i);
    if (n.a >= 0) res[std::size_t(n.a)].last = int(i);
    if (n.b >= 0) res[std::size_t(n.b)].last = int(i);
  }
  if (nodes.size() + 1 > 512) return err = "too many passes in one frame", Status::InvalidArgument;

  struct Dirty {
    std::uint64_t id;
    char by;  // 'i' compute wrote an image, 'b' compute wrote a buffer, 't' a copy wrote it
  };
  std::vector<Dirty> dirty;
  std::vector<std::uint64_t> readSince;
  auto findDirty = [&](std::uint64_t id) -> const Dirty* {
    for (const Dirty& d : dirty)
      if (d.id == id) return &d;
    return nullptr;
  };
  auto hasRead = [&](std::uint64_t id) { return std::find(readSince.begin(), readSince.end(), id) != readSince.end(); };
  const double split = dev.splitGroups();
  double workSince = 0;
  const bool prof = dev.caps().timestamps;

  auto resolve = [&](H h) -> Status {
    auto& r = res[std::size_t(h)];
    if (r.pooled && !r.hasImg) {
      const Status s = g.pool_.acquire(r.fmt, r.w, r.h, &r.img);
      if (s != Status::Ok) return err = dev.lastError().empty() ? "out of memory for a working image" : dev.lastError(), s;
      r.hasImg = true;
    }
    return Status::Ok;
  };
  auto img = [&](H h) { return res[std::size_t(h)].img; };
  auto bufOf = [&](H h) { return res[std::size_t(h)].buf; };

  for (std::size_t ni = 0; ni < nodes.size(); ++ni) {
    const GpuGraph::Node& n = nodes[std::size_t(order[ni])];
    const auto acc = accesses(n);
    for (const Acc& a : acc) {
      const Status s = resolve(a.h);
      if (s != Status::Ok) return s;
    }
    // barrier class mask from the real dependencies of this pass (consumer oriented)
    std::uint32_t mask = 0;
    const bool consumerIsDispatch = n.kind == GpuGraph::Node::Dispatch;
    for (const Acc& a : acc) {
      const std::uint64_t id = keyOf(a.h);
      if (const Dirty* d = findDirty(id)) {
        const bool isImg = res[std::size_t(a.h)].hasImg;
        if (d->by == 't') mask |= consumerIsDispatch ? hal::kTransferToCompute : (hal::kTransferToCompute | hal::kComputeToTransfer);
        else if (!consumerIsDispatch) mask |= hal::kComputeToTransfer;
        else if (isImg) mask |= a.bind == 'S' ? hal::kImgStoreToSampled : hal::kImgStoreToImgLoad;
        else mask |= hal::kBufWriteToBufRead;
        if (a.wr && !a.rd) mask |= hal::kImgStoreToImgLoad | hal::kBufWriteToBufRead;  // WAW
      } else if (a.wr && hasRead(id)) {
        mask |= hal::kAll;  // WAR
      }
    }
    if (mask) {
      cl.barrier(mask);
      dirty.clear();
      readSince.clear();
    }
    for (const Acc& a : acc) {
      const std::uint64_t id = keyOf(a.h);
      if (a.wr) {
        const bool isImg = res[std::size_t(a.h)].hasImg;
        dirty.push_back({id, a.bind == 'T' ? 't' : isImg ? 'i' : 'b'});
      }
      if (a.rd && !hasRead(id)) readSince.push_back(id);
    }

    switch (n.kind) {
      case GpuGraph::Node::Dispatch: {
        cl.bindPipeline(n.pl);
        for (std::size_t i = 0; i < n.uses.size(); ++i) {
          const auto& r = res[std::size_t(n.uses[i].res)];
          if (r.hasImg) cl.bind(std::uint32_t(i), r.img);
          else cl.bind(std::uint32_t(i), r.buf);
        }
        Status s = cl.setParams(n.params.data(), n.params.size());
        if (s == Status::Ok) s = cl.dispatch(n.gx, n.gy, n.gz);
        if (s != Status::Ok) return err = dev.lastError().empty() ? std::string("dispatch of ") + n.name + " failed" : dev.lastError(), s;
        workSince += double(n.gx) * n.gy * n.gz * n.weight;
        break;
      }
      case GpuGraph::Node::BufToImg: cl.copyBufferToImage(n.buf, img(n.b), n.w, n.h, n.d, n.off); break;
      case GpuGraph::Node::BufToBuf: cl.copyBufferToBuffer(n.buf, bufOf(n.b), n.bytes, n.off); break;
      case GpuGraph::Node::ImgToImg: cl.copyImage(img(n.a), img(n.b), n.w, n.h); break;
      case GpuGraph::Node::ImgToBuf: cl.copyImageToBuffer(img(n.a), n.buf, hal::Rect2{n.x, n.y, n.w, n.h}, 0); break;
    }
    if (prof) cl.timestamp(n.name.c_str());
    // Long frames are cut into several submissions (Adreno kgsl hang detection, pixel-architecture.md 16 rule 1).
    if (workSince >= split && ni + 1 < nodes.size()) {
      const Status s = cl.flushPart();
      if (s != Status::Ok) return err = dev.lastError().empty() ? "submission split failed" : dev.lastError(), s;
      dirty.clear();
      readSince.clear();
      workSince = 0;
    }
    for (auto& r : res)
      if (r.pooled && r.hasImg && r.last == int(ni)) {
        g.pool_.release(r.img);
        r.hasImg = false;
        r.last = -2;
      }
  }
  guard.ok = true;
  return cl.status();
}

Status runGraph(hal::Device& dev, GpuGraph& g, hal::CommandList& cl, hal::FenceH* fence, std::string& err) {
  Status s = submitGraph(g, cl, err);
  if (s != Status::Ok) {
    hal::FenceH f;
    dev.submit(cl, &f);  // closes the recording; the error status keeps it from reaching the queue
    return s;
  }
  s = dev.submit(cl, fence);
  if (s != Status::Ok) err = dev.lastError();
  return s;
}

}  // namespace dapple::engine::gpu
