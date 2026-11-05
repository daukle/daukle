daukle.source{
  name = "generated",
  load = function(project, block, base_dir)
    return {
      schema = 1,
      project = project,
      version = "5.0.0",
      modules = { ir = { plaintext = {} } },
    }
  end,
}
