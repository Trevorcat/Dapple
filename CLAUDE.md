# Dapple 项目规则

## Subagent 只能用 Sonnet

凡是由 Claude 发起的 subagent，一律使用 Sonnet，没有例外：

- Agent 工具：每次调用都显式传 `model: "sonnet"`。
- Workflow 脚本：每个 `agent()` 调用都传 `model: 'sonnet'`。
- 不使用 `subagent_type: "fork"`（fork 会继承主会话的模型，无法改成 Sonnet）。
- subagent 再派生的 subagent 同样只能是 Sonnet；给 subagent 的任务说明里要写明这一条。

主会话负责验收和指导修复，实现工作交给 Sonnet subagent。
