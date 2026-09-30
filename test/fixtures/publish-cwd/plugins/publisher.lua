daukle.plugin{ api = 1, uses = { "tool", "exec", "env" } }

daukle.publisher{
  name = "somewhere",
  publish = function(context)
    local child = daukle.tool(daukle.env("DAUKLE_TEST_CHILD"))
    daukle.exec(child, { "--task-child" })
  end,
}
