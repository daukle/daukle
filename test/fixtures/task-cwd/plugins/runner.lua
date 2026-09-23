daukle.plugin{ api = 1, uses = { "exec", "tool", "env" } }

daukle.toolchain{
  name = "runner",
  generate = function() return {} end,
}

daukle.task{
  name = "runner:touch",
  run = function()
    daukle.exec(daukle.tool(daukle.env("DAUKLE_TEST_CHILD")), { "--task-child" })
  end,
}
