daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "gradle",
  generate = function(toolchain)
    local lines = {}
    for index = 1, #toolchain.dependencies do
      lines[index] = toolchain.dependencies[index].block.coordinate
    end
    return { ["dependencies.txt"] = table.concat(lines, "\n") .. "\n" }
  end,
}
