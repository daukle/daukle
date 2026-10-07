-- Declares GITHUB_TOKEN, so the child this task starts must see it.
daukle.plugin{ api = 1, uses = { "exec", "tool", "env" }, env = { "GITHUB_TOKEN" } }

daukle.toolchain{
  name = "runner",
  generate = function() return {} end,
}

daukle.task{
  name = "runner:touch",
  run = function()
    daukle.exec(daukle.tool(daukle.env("DAUKLE_TEST_CHILD")), { "--task-child-env" })
  end,
}
