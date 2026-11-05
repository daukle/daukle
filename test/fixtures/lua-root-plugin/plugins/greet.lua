daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "greet",
  generate = function(toolchain)
    local lines = {}
    for index = 1, #toolchain.dependencies do
      local dependency = toolchain.dependencies[index]
      lines[index] = "greet " .. dependency.project .. "/" .. dependency.module
    end
    return { ["greet.txt"] = table.concat(lines, "\n") .. "\n" }
  end,
}
