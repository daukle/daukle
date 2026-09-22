daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    return { ["a.txt"] = toolchain.config.leak }
  end,
}
