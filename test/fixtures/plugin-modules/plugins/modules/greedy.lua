daukle.plugin{ api = 2, uses = { "region" } }

local borrowed = daukle.write

daukle.source{
  name = "modular-greedy",
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = "1.0.0", modules = {} }
  end,
}
