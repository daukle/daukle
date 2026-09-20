daukle.plugin{ api = 1, uses = {} }

daukle.language{
  name = "greet",
  apply = function(consumer, resolved, text)
    local lines = {}
    for index = 1, #resolved do
      lines[index] = "greet " .. resolved[index].project .. "/" .. resolved[index].module
    end
    return table.concat(lines, "\n") .. "\n"
  end,
}
