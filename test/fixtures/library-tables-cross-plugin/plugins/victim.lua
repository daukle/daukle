daukle.plugin{ api = 2, uses = {} }

daukle.toolchain{
  name = "victim",
  generate = function(context) return {} end,
}

daukle.task{
  name = "victim:check",
  run = function(context)
    local direct = string.format("%d", 7)
    local method = ("%d"):format(8)
    if direct ~= "7" or method ~= "8" then
      error("a planted function ran: " .. tostring(direct) .. " " .. tostring(method), 0)
    end
  end,
}
