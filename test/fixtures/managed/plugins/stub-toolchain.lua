daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    return { ["build.txt"] = "target " .. tostring(toolchain.config.target) .. "\n" }
  end,
}
