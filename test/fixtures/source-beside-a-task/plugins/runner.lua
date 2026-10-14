daukle.plugin{ api = 2, uses = { "exec", "tool", "env" } }

daukle.source{
  name = "execing",
  load = function(project, block, base_dir)
    daukle.exec(daukle.tool(daukle.env("DAUKLE_TEST_CHILD")), { "--task-child" })
  end,
}

daukle.language{
  name = "passthrough",
  apply = function(consumer, resolved, text) return text end,
}

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
