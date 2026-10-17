daukle.plugin{ api = 3 }

daukle.toolchain{
  name = "echo",
  generate = function(context)
    local keys = {}
    for key in pairs(context.config) do keys[#keys + 1] = key end
    table.sort(keys)
    return { ["root.txt"] = context.root .. "\n" .. table.concat(keys, ",") .. "\n" }
  end,
}
