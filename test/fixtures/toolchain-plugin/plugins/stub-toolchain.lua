daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    return {
      ["generated.txt"] = "project " .. toolchain.project
                          .. "\nversion " .. toolchain.version
                          .. "\ntarget " .. tostring(toolchain.config.target)
                          .. "\nroot " .. toolchain.root
                          .. "\nos " .. toolchain.host.os .. "\n",
    }
  end,
}
