daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "stub",
  generate = function(context)
    local lines = {}
    for index = 1, #context.modules do
      local module = context.modules[index]
      lines[#lines + 1] = module.name .. "<" .. table.concat(module.requires, ",")
    end
    table.sort(lines)
    return { ["modules.txt"] = table.concat(lines, "\n") .. "\n" }
  end,
}
