# Dapple 项目规则

## Subagent 的模型与并发

凡是由 Claude 发起的 subagent，都要显式指定模型，不依赖默认继承：

- 尽量用 Sonnet：Agent 工具传 `model: "sonnet"`，Workflow 脚本的每个 `agent()` 传 `model: 'sonnet'`。
- Opus（`model: "opus"`）只用于确实需要的任务：Sonnet 做过一轮没做好，或者设计判断、成像算法取舍、并发崩溃根因这类出错代价大的工作。
- 同时运行的 subagent 不超过 3 路（subagent 自己再派生的也算在内）。任务说明里要写明：不再派生 subagent，或最多派生几个。
- 不使用 `subagent_type: "fork"`（fork 继承主会话的模型，无法指定）。
- 任务说明要省 token：只看日志尾部，不重复读大文件，不做探索性的重复运行。

主会话负责验收和指导修复，实现工作交给 subagent。汇报进度前先核实 subagent 的实际状态，不转述旧的中间消息。
