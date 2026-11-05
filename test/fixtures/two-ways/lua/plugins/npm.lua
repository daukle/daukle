daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "npm",
  generate = function(toolchain)
    local lines = {}
    for index = 1, #toolchain.dependencies do
      local block = toolchain.dependencies[index].block
      lines[index] = block.package .. " " .. block.range
    end
    return { ["dependencies.txt"] = table.concat(lines, "\n") .. "\n" }
  end,
}
