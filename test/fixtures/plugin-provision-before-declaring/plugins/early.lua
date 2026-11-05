daukle.plugin{ api = 1, uses = { "provision" } }

daukle.provision{
  url    = "http://127.0.0.1:1/never-asked.tar.gz",
  sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
}

daukle.source{
  name = "early",
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = "1.0.0", modules = {} }
  end,
}
