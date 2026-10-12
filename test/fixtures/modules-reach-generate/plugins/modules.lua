daukle.plugin{ api = 1, uses = { "write" } }

local function list_modules(context)
  local lines = {}
  for index = 1, #context.modules do
    local module = context.modules[index]
    assert(module.blocks == nil, "a module's blocks must not reach a plugin")
    lines[#lines + 1] = module.name .. "<" .. table.concat(module.requires, ",")
  end
  table.sort(lines)
  return table.concat(lines, "\n") .. "\n"
end

daukle.toolchain{
  name = "stub",
  generate = function(context)
    return { ["modules.txt"] = list_modules(context) }
  end,
}

daukle.task{
  name = "stub:list",
  run = function(context)
    daukle.write{ path = "task-modules.txt", text = list_modules(context) }
  end,
}
