daukle.plugin{ api = 1, uses = { "tool" } }

daukle.source{
  name = "ok",
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = "1.0.0", modules = {} }
  end,
}
