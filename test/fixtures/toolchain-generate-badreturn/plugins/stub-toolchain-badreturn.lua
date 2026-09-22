daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    return "not a table"
  end,
}
