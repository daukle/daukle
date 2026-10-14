daukle.plugin{ api = 2, uses = { "provision" } }

local function fetch_something()
  return daukle.provision({})
end

daukle.source{
  name = "path",
  load = function(project, block, base_dir)
    fetch_something()
  end,
}
