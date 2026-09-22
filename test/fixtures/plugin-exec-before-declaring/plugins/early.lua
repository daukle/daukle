daukle.plugin{ api = 1, uses = { "exec" } }

daukle.exec(nil, {})

daukle.language{
  name = "early",
  apply = function(consumer, resolved, text) return text end,
}
