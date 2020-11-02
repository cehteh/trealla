:- initialization(main).

main :-
	JsonData = '[{"foo": 1, "bar": 2}, {"bar": 3, "foo": 4}]',
	read_term_from_chars(JsonData, [double_quotes(atom)], Data),
	findall(X, (member({F1:A, F2:B},Data), (F1=foo -> X = A ; (F2=foo -> X = B))), L),
	writeln(L),
	halt.
