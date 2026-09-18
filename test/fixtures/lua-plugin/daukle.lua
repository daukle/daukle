daukle.language{
  name = "plaintext",
  apply = function(consumer, resolved, text)
    local lines = {}
    for index = 1, #resolved do
      lines[index] = resolved[index].project .. "/" .. resolved[index].module
    end
    return table.concat(lines, "\n") .. "\n"
  end,
}

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
