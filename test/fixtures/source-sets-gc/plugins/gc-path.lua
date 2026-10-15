daukle.plugin{ api = 2, uses = { "exec", "tool" } }

daukle.source{
  name = "path",
  load = function(project, block, base_dir)
    setmetatable({}, { __gc = function() end })
  end,
}
