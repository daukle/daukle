daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "hello",
  generate = function(toolchain)
    local lines = {}
    for index = 1, #toolchain.dependencies do
      local dependency = toolchain.dependencies[index]
      lines[index] = dependency.project .. "/" .. dependency.module
    end
    return { ["hello.txt"] = table.concat(lines, "\n") .. "\n" }
  end,
}
