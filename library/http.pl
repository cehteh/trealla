:- module(http, [
	http_open/3, http_get/3, http_post/4, http_put/4, http_delete/3,
	http_request/5
	]).

:- use_module(library(dict)).

read_response(S, Code) :-
	getline(S, Line),
	split(Line, ' ' ,_Ver, Rest),
	split(Rest, ' ', Code2, _Rest2),
	number_chars(Code, Code2).

read_header(S, Pair) :-
	getline(S, Line),
	split(Line,':', K, V),
	(K \= '' -> true ; (!, fail)),
	string_lower(K, K2),
	Pair=K2:V.
read_header(S, Pair) :-
	\+ at_end_of_stream(S),
	read_header(S, Pair).

read_chunks(S, Tmp, Data) :-
	getline(S, Line),
	hex_chars(Len, Line),
	Len > 0,
	bread(S, Len, Tmp2),
	getline(S, _),
	append(Tmp, Tmp2, Tmp3),
	read_chunks(S, Tmp3, Data).
read_chunks(_, Data, Data).

read_body(S, Hdrs, Data) :-
	dict:get(Hdrs, 'content-length', V, _),
	number_chars(Len, V),
	bread(S, Len, Data).

% Open with options...

http_open([], _, _) :- !, fail.
http_open(UrlList, S, Opts) :-
	is_list(UrlList),
	is_list(Opts),
	union(UrlList, Opts, OptList),
	memberchk(host(Host), UrlList),
	memberchk(path(Path), UrlList),
	(memberchk(method(Method), OptList) ->
		true ;
		Method = get),
	(memberchk(version(Maj-Min), OptList) ->
		true ;
		(Maj = 1, Min = 1)),
	client(Host, _Host, _Path, S, OptList),
	string_upper(Method, UMethod),
	format(S,"~a /~a HTTP/~d.~d\r\nHost: ~a\r\nConnection: keep-alive\r\n\r\n", [UMethod, Path, Maj, Min, Host]),
	read_response(S, Code),
	findall(Hdr, read_header(S, Hdr), Hdrs),
	append(Host, Path, Url),
	dict:get(Hdrs, 'location', Location, Url),
	ignore(memberchk(status_code(Code), OptList)),
	ignore(memberchk(headers(Hdrs), OptList)),
	ignore(memberchk(final_url(Location), OptList)).

% Client request processing...

process(Url, S, Opts) :-
	is_list(Opts),
	OptList=Opts,
	(memberchk(post(PostData), OptList) ->
		Method2 = post ;
		Method2 = get),
	(memberchk(method(Method), OptList) ->
		true ;
		Method = Method2),
	(memberchk(version(Maj-Min), OptList) ->
		true ;
		(Maj = 1, Min = 1)),
	client(Url, Host, Path, S, OptList),
	string_upper(Method, UMethod),
	(memberchk(header('content_type', Ct), OptList) ->
		format(atom(Ctype), "Content-Type: ~w\r\n",[Ct]) ;
		Ctype = '' ),
	(nonvar(PostData) ->
		(length(PostData, DataLen), format(atom(Clen), "Content-Length: ~d\r\n", [DataLen])) ;
		Clen = '' ),
	format(S,"~a /~a HTTP/~d.~d\r\nHost: ~a\r\nConnection: close\r\n~w~w\r\n", [UMethod, Path, Maj, Min, Host, Ctype, Clen]),
	(nonvar(DataLen) -> bwrite(S, PostData) ; true),
	read_response(S, Code),
	findall(Hdr, read_header(S, Hdr), Hdrs),
	ignore(memberchk(status_code2(Code), OptList)),
	ignore(memberchk(headers2(Hdrs), OptList)).

% Client requests...

http_get(Url, Data, Opts) :-
	Opts2=[headers2(Hdrs)|Opts],
	Opts3=[status_code2(Code)|Opts2],
	process(Url, S, Opts3),
	dict:get(Hdrs, 'transfer-encoding', TE, ''),
	( TE == chunked ->
		read_chunks(S, '', Body) ;
		read_body(S, Hdrs, Body)
	),
	close(S),
	(memberchk(Code, [301,302]) ->
		(dict:get(Hdrs, 'location', Loc, ''),
		http_get(Loc, Data, Opts))
	;
		(Data=Body,
		ignore(memberchk(final_url(Url), Opts)),
		ignore(memberchk(status_code(Code), Opts)),
		ignore(memberchk(headers(Hdrs), Opts)))
	).

http_post(Url, Data, Reply, Opts) :-
	http_get(Url, Reply,[post(Data)|Opts]).

http_put(Url, Data, Reply, Opts) :-
	http_post(Url, Data, Reply, [method(put)|Opts]).

http_delete(Url, Data, Opts) :-
	http_get(Url, Data, [method(delete)|Opts]).

% Handle a server request...

http_request(S, Method, Path, Ver, Hdrs) :-
	getline(S, Line),
	split(Line, ' ' ,Method2, Rest),
	split(Rest, ' ', Path, Rest2),
	split(Rest2, '/', _, Ver),
	string_upper(Method2, Method),
	findall(Hdr, read_header(S, Hdr), Hdrs).
