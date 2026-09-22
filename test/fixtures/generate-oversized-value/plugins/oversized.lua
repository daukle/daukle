daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    return { ["a.txt"] = string.rep("x", 1024 * 1024 + 1) }
  end,
}
