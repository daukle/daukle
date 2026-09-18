daukle.source{
  name = "erroring",
  load = function(project, block, base_dir)
    error("boom from a source plugin")
  end,
}
