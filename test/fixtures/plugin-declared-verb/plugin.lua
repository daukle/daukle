daukle.plugin{ api = 1, uses = { "env" } }

local home = daukle.env("PATH")

daukle.source{
  name = "brewfile",
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = tostring(home ~= nil), modules = {} }
  end,
}
