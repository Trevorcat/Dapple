# Dapple 项目规则

## Subagent 的模型

凡是由 Claude 发起的 subagent，都要显式指定模型，不依赖默认继承：

- 默认用 Sonnet：Agent 工具传 `model: "sonnet"`，Workflow 脚本的每个 `agent()` 传 `model: 'sonnet'`。
- 可以用 Opus（`model: "opus"`）：难度高、Sonnet 做过一轮没做好、或者出错代价大的任务，例如并发崩溃的根因分析、成像算法和画质取舍、跨模块的架构改动。常规实现、打包、界面改动、测试和文档仍用 Sonnet。
- 不使用 `subagent_type: "fork"`（fork 继承主会话的模型，无法指定）。
- subagent 再派生的 subagent 遵守同一条规则；给 subagent 的任务说明里要写明。

主会话负责验收和指导修复，实现工作交给 subagent。
