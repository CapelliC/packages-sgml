/*  $Id$

    Part of SWI-Prolog SGML/XML parser

    Author:  Jan Wielemaker
    E-mail:  jan@swi.psy.uva.nl
    WWW:     http://www.swi.psy.uva.nl/projects/SWI-Prolog/
    Copying: LGPL-2.  See the file COPYING or http://www.gnu.org

    Copyright (C) 1990-2002 SWI, University of Amsterdam. All rights reserved.
*/

:- module(rdf_w3c_test,
	  [ process_manifest/0,
	    process_manifest/1,
	    run_tests/0,		% run all tests
	    run/0,			% run selected test
	    show/1,			% RDF diagram for File
	    run_test/1			% run a single test
	  ]).
:- use_module(rdf).			% our RDF parser
:- use_module(rdf_nt).			% read .nt files
:- use_module(library(pce)).
:- use_module(library(toolbar)).
:- use_module(library(pce_report)).
:- use_module(rdf_diagram).

:- dynamic
	rdf/3.

ns(test,
   'http://www.w3.org/2000/10/rdf-tests/rdfcore/testSchema/').

local('http://www.w3.org/2000/10/rdf-tests/rdfcore/',
      'W3Ctests/').

process_manifest :-
	process_manifest('W3Ctests/Manifest.rdf').
process_manifest(Manifest) :-
	retractall(rdf(_,_,_)),
	load_rdf(Manifest, Triples),
	assert_triples(Triples).

assert_triples([]).
assert_triples([rdf(S, P, O)|T]) :-
	canonise(S, Subject),
	canonise(P, Predicate),
	canonise(O, Object),
	assert(rdf(Subject, Predicate, Object)),
	assert_triples(T).

canonise(NS:Name, N:Name) :-
	ns(N, NS), !.
canonise(Absolute, N:Name) :-
	atom(Absolute),
	ns(N, NS),
	atom_concat(NS, Name, Absolute), !.
canonise(X, X).
	

run_tests :-
	start_tests,
	(   rdf(About, rdf:type, test:Type),
	    test_type(Type),
%	    once(run_test(About)),		% Should not be needed
	    run_test(About),
	    fail
	;   true
	), !,
	report_results.

test_type('PositiveParserTest').
%test_type('NegativeParserTest').

run_test(Test) :-
	rdf(Test, test:inputDocument, In),
	local_file(In, InFile),
	exists_file(InFile),
	rdf(Test, test:outputDocument, Out),
	local_file(Out, NTFile),
	load_rdf(InFile, RDF,
		 [ base_uri(In),
		   expand_foreach(true)
		 ]),
	load_rdf_nt(NTFile, NT),
	(   compare_triples(RDF, NT)
	->  test_result(pass, Test, RDF, NT)
	;   test_result(fail, Test, RDF, NT)
	).


local_file(URL, File) :-
	local(URLPrefix, FilePrefix),
	atom_concat(URLPrefix, Base, URL), !,
	atom_concat(FilePrefix, Base, File).


		 /*******************************
		 *	       GUI		*
		 *******************************/

:- pce_begin_class(w3c_rdf_test_gui, frame).

initialise(F) :->
	send_super(F, initialise, 'W3C RDF test suite results'),
	send(F, append, new(D, tool_dialog(F))),
	send(new(B, browser), below, D),
	send(F, fill_menu, D),
	send(F, fill_browser, B),
	send(new(report_dialog), below, B).

fill_menu(F, D:tool_dialog) :->
	send_list(D,
		  [ append(menu_item(exit, message(F, destroy)),
			   file)
		  ]).

fill_browser(_F, B:browser) :->
	send(B, style, pass, style(colour := dark_green)),
	send(B, style, fail, style(colour := red)),
	send(B?image, recogniser,
	     handler(ms_right_down,
		     and(message(B, selection,
				 ?(B, dict_item, @event)),
			 new(or)))),
	send(B, popup, new(P, popup)),
	send_list(P, append,
		  [ menu_item(edit,
			      message(@arg1, edit_test)),
		    menu_item(run,
			      message(@arg1, run)),
		    gap,
		    menu_item(show_result,
			      message(@arg1, show_triples, result)),
		    menu_item(show_norm,
			      message(@arg1, show_triples, norm)),
		    gap,
		    menu_item(discussion,
			      message(@arg1, open_url, discussion),
			      condition :=
			      message(@arg1, has_url, discussion)),
		    menu_item(approval,
			      message(@arg1, open_url, approval),
			      condition :=
			      message(@arg1, has_url, approval)),
		    gap,
		    menu_item(copy_test_uri,
			      message(@arg1, copy_test_uri))
		  ]).


test_result(F, Result:{pass,fail}, Test:name, Our:prolog, Norm:prolog) :->
	"Test failed"::
	Data = [ result(Our),
		 norm(Norm)
	       ],
	get(F, member, browser, B),
	(   get(B, member, Test, Item)
	->  send(Item, object, prolog(Data)),
	    send(Item, style, Result)
	;   send(B, append,
		 rdf_test_item(Test, @default, prolog(Data), Result))
	).

clear(F) :->
	get(F, member, browser, B),
	send(B, clear).

summarise(F) :->
	get(F, member, browser, Browser),
	new(Pass, number(0)),
	new(Fail, number(0)),
	send(Browser?members, for_all,
	     if(@arg1?style == pass,
		message(Pass, plus, 1),
		message(Fail, plus, 1))),
	send(F, report, status, '%d tests succeeded; %d failed',
	     Pass, Fail).

:- pce_end_class(w3c_rdf_test_gui).

:- pce_begin_class(rdf_test_item, dict_item).


edit_test(Item) :->
	"Edit input document of test"::
	get(Item, key, Test),
	rdf(Test, test:inputDocument, In),
	local_file(In, InFile),
	edit(file(InFile)).

show_triples(Item, Set:{result,norm}) :->
	"Show result of our parser"::
	get(Item, key, Test),
	get(Item, object, List),
	Term =.. [Set,Triples],
	member(Term, List),
	send(Item, show_diagram(Triples,
				string('%s for %s', Set?label_name, Test))).

show_diagram(_Item, Triples:prolog, Label:name) :->
	"Show diagram for triples"::
	new(D, rdf_diagram(Label)),
	send(new(report_dialog), below, D),
	forall(member(T, Triples),
	       send(D, append, T)),
	send(D, layout),
	send(D, open).

open_url(Item, Which:name) :->
	"Open associated URL in browser"::
	get(Item, key, Test),
	rdf(Test, test:Which, URL),
	www_open_url(URL).

has_url(Item, Which:name) :->
	"Test if item has URL"::
	get(Item, key, Test),
	rdf(Test, test:Which, _URL).

run(Item) :->
	"Re-run the test"::
	get(Item, key, Test),
	run_test(Test).

copy_test_uri(Item) :->
	"Copy URI of test to clipboard"::
	get(Item, key, Test),
	send(@display, copy, Test).

:- pce_end_class(rdf_test_item).


:- pce_global(@rdf_test_gui, make_rdf_test_gui).

make_rdf_test_gui(Ref) :-
	send(new(Ref, w3c_rdf_test_gui), open).


:- dynamic
	verbose/0.

verbose.

test_result(Result, Test, Our, Norm) :-
	send(@rdf_test_gui, test_result, Result, Test, Our, Norm),
	(   Result == fail, verbose
	->  length(Our, OurLength),
	    format('~N** Our Triples (~w)~n', OurLength),
	    pp(Our),
	    length(Norm, NormLength),
	    format('~N** Normative Triples (~w)~n', NormLength),
	    pp(Norm)
	;   true
	).
	    


start_tests :-
	send(@rdf_test_gui, clear).

report_results :-
	send(@rdf_test_gui, summarise).

run :-
	get(@rdf_test_gui, member, browser, B),
	get(B, selection, DI),
	get(DI, key, Test),
	run_test(Test).


		 /*******************************
		 *	     SHOW A FILE	*
		 *******************************/


show(File) :-
	absolute_file_name(File,
			   [ access(read),
			     extensions([rdf,rdfs,owl,''])
			   ], AbsFile),
	load_rdf(AbsFile, Triples,
		 [ expand_foreach(true)
		 ]),
	new(D, rdf_diagram(string('RDF diagram for %s', File))),
	send(new(report_dialog), below, D),
	forall(member(T, Triples),
	       send(D, append, T)),
	send(D, layout),
	send(D, open).

	

		 /*******************************
		 *	     COMPARING		*
		 *******************************/

compare_triples(A, B) :-
	compare_list(A, B, [], _).

compare_list([], [], S, S).
compare_list([H1|T1], In2, S0, S) :-
	select(H2, In2, T2),
	compare_triple(H1, H2, S0, S1), !, % put(.), flush_output,
	compare_list(T1, T2, S1, S).

compare_triple(rdf(Subj1,P1,O1), rdf(Subj2, P2, O2), S0, S) :-
	compare_field(Subj1, Subj2, S0, S1),
	compare_field(P1, P2, S1, S2),
	compare_field(O1, O2, S2, S).

compare_field(X, X, S, S) :- !.
compare_field(literal(X), xml(X), S, S) :- !. % TBD
compare_field(rdf:Name, Atom, S, S) :-
	rdf_parser:rdf_name_space(NS),
	atom_concat(NS, Name, Atom), !.
compare_field(NS:Name, Atom, S, S) :-
	atom_concat(NS, Name, Atom), !.
compare_field(X, node(Id), S, S) :-
	memberchk(X=Id, S), !.
compare_field(X, node(Id), S, [X=Id|S]) :-
	\+ memberchk(X=_, S),
	atom(X),
	generated_prefix(Prefix),
	sub_atom(X, 0, _, _, Prefix),
	format('Assume ~w = ~w~n', [X, node(Id)]).

generated_prefix('Bag__').
generated_prefix('Seq__').
generated_prefix('Alt__').
generated_prefix('Description__').
generated_prefix('Statement__').
