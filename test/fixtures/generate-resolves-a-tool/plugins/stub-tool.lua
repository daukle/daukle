daukle.plugin{ api = 1, uses = { "tool" } }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    daukle.tool("some-name")
    return {}
  end,
}
