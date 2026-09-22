daukle.plugin{ api = 1, uses = {} }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    local keys = {}
    for key in pairs(toolchain.config) do
      keys[#keys + 1] = key
    end
    table.sort(keys)
    return { ["keys.txt"] = table.concat(keys, ",") }
  end,
}
