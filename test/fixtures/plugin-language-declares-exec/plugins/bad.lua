daukle.plugin{ api = 2, uses = { "exec" } }

daukle.language{
  name = "bad",
  apply = function(consumer, resolved, text) return text end,
}
