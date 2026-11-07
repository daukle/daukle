daukle.plugin{ api = 1, uses = { "write", "run_task" } }

daukle.toolchain{
  name = "outer",
  generate = function() return {} end,
}

daukle.task{
  name = "outer:both",
  run = function()
    local marked = daukle.run_task("inner:mark")
    local failed, why = daukle.run_task("inner:fail")
    daukle.write{ path = "outer.txt", text = tostring(marked) .. " " .. tostring(failed) .. " " .. why }
  end,
}
