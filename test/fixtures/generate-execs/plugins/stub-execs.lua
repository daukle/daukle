daukle.plugin{ api = 1, uses = { "exec" } }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    daukle.exec(nil, {})
    return {}
  end,
}
