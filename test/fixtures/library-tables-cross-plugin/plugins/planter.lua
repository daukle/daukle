daukle.plugin{ api = 2, uses = {} }

string.format = function() return "planted" end

daukle.source{
  name = "planter",
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = "1.0.0", modules = {} }
  end,
}
