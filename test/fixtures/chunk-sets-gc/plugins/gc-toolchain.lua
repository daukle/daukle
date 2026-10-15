daukle.plugin{ api = 2, uses = { "exec", "tool" } }

setmetatable({}, { __gc = function() end })

daukle.toolchain{
  name = "gc",
  generate = function(context) return {} end,
}
