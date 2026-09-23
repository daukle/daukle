local greeting = {}

greeting.name = "hello"

function greeting.render(resolved)
  local lines = {}
  for index = 1, #resolved do
    lines[index] = resolved[index].project .. "/" .. resolved[index].module
  end
  return table.concat(lines, "\n") .. "\n"
end

return greeting
