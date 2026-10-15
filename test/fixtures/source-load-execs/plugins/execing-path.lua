daukle.plugin{ api = 2, uses = { "exec" } }

local function run_something()
  return daukle.exec()
end

daukle.source{
  name = "path",
  load = function(project, block, base_dir)
    run_something()
  end,
}
