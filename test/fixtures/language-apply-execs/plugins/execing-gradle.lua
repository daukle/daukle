daukle.plugin{ api = 2, uses = { "exec" } }

daukle.language{
  name = "gradle",
  apply = function(consumer, resolved, text)
    daukle.exec()
    return text
  end,
}
