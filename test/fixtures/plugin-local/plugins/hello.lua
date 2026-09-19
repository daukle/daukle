daukle.plugin{ api = 1, uses = {} }

daukle.language{
  name = "hello",
  apply = function(consumer, resolved, text)
    local lines = {}
    for index = 1, #resolved do
      lines[index] = resolved[index].project .. "/" .. resolved[index].module
    end
    return table.concat(lines, "\n") .. "\n"
  end,
}
