%%
%% %CopyrightBegin%
%%
%% Copyright Ericsson AB 2010. All Rights Reserved.
%%
%% The contents of this file are subject to the Erlang Public License,
%% Version 1.1, (the "License"); you may not use this file except in
%% compliance with the License. You should have received a copy of the
%% Erlang Public License along with this software. If not, it can be
%% retrieved online at http://www.erlang.org/.
%%
%% Software distributed under the License is distributed on an "AS IS"
%% basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
%% the License for the specific language governing rights and limitations
%% under the License.
%%
%% %CopyrightEnd%
%%

%% Create test certificates

-module(erl_make_certs).
-include_lib("public_key/include/public_key.hrl").

-export([make_cert/1, gen_rsa/1, verify_signature/3, write_pem/3]).
-compile(export_all).

%%--------------------------------------------------------------------
%% @doc  Create and return a der encoded certificate
%%   Option                                         Default
%%   -------------------------------------------------------
%%   digest                                         sha1
%%   validity                                       {date(), date() + week()}
%%   version                                        3
%%   subject                                        [] list of the following content
%%      {name,  Name}
%%      {email, Email} 
%%      {city,  City}
%%      {state, State}
%%      {org, Org}
%%      {org_unit, OrgUnit}
%%      {country, Country} 
%%      {serial, Serial}
%%      {title, Title}
%%      {dnQualifer, DnQ}
%%   issuer = {Issuer, IssuerKey}                   true (i.e. a ca cert is created) 
%%                                                  (obs IssuerKey migth be {Key, Password}
%%   key = KeyFile|KeyBin|rsa|dsa                   Subject PublicKey rsa or dsa generates key
%%   
%%
%%   (OBS: The generated keys are for testing only)
%% @spec ([{::atom(), ::term()}]) -> {Cert::binary(), Key::binary()}
%% @end
%%--------------------------------------------------------------------

make_cert(Opts) ->
    SubjectPrivateKey = get_key(Opts),
    {TBSCert, IssuerKey} = make_tbs(SubjectPrivateKey, Opts),
    Cert = public_key:pkix_sign(TBSCert, IssuerKey),
    true = verify_signature(Cert, IssuerKey, undef), %% verify that the keys where ok
    {Cert, encode_key(SubjectPrivateKey)}.

%%--------------------------------------------------------------------
%% @doc Writes pem files in Dir with FileName ++ ".pem" and FileName ++ "_key.pem"
%% @spec (::string(), ::string(), {Cert,Key}) -> ok
%% @end
%%--------------------------------------------------------------------
write_pem(Dir, FileName, {Cert, Key = {_,_,not_encrypted}}) when is_binary(Cert) ->
    ok = ssl_test_lib:der_to_pem(filename:join(Dir, FileName ++ ".pem"), 
			       [{'Certificate', Cert, not_encrypted}]),
    ok = ssl_test_lib:der_to_pem(filename:join(Dir, FileName ++ "_key.pem"), [Key]).

%%--------------------------------------------------------------------
%% @doc Creates a rsa key (OBS: for testing only)
%%   the size are in bytes
%% @spec (::integer()) -> {::atom(), ::binary(), ::opaque()}
%% @end
%%--------------------------------------------------------------------
gen_rsa(Size) when is_integer(Size) ->
    Key = gen_rsa2(Size),
    {Key, encode_key(Key)}.

%%--------------------------------------------------------------------
%% @doc Creates a dsa key (OBS: for testing only)
%%   the sizes are in bytes
%% @spec (::integer()) -> {::atom(), ::binary(), ::opaque()}
%% @end
%%--------------------------------------------------------------------
gen_dsa(LSize,NSize) when is_integer(LSize), is_integer(NSize) ->
    Key = gen_dsa2(LSize, NSize),
    {Key, encode_key(Key)}.

%%--------------------------------------------------------------------
%% @doc Verifies cert signatures
%% @spec (::binary(), ::tuple()) -> ::boolean()
%% @end
%%--------------------------------------------------------------------
verify_signature(DerEncodedCert, DerKey, _KeyParams) ->
    Key = decode_key(DerKey),
    case Key of 
	#'RSAPrivateKey'{modulus=Mod, publicExponent=Exp} ->
	    public_key:pkix_verify(DerEncodedCert, 
				   #'RSAPublicKey'{modulus=Mod, publicExponent=Exp});
	#'DSAPrivateKey'{p=P, q=Q, g=G, y=Y} ->
	    public_key:pkix_verify(DerEncodedCert, {Y, #'Dss-Parms'{p=P, q=Q, g=G}})
    end.

%%%%%%%%%%%%%%%%%%%%%%%%% Implementation %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

get_key(Opts) ->
    case proplists:get_value(key, Opts) of
	undefined -> make_key(rsa, Opts);
	rsa ->       make_key(rsa, Opts);
	dsa ->       make_key(dsa, Opts);
	Key ->
	    Password = proplists:get_value(password, Opts, no_passwd),
	    decode_key(Key, Password)
    end.

decode_key({Key, Pw}) ->
    decode_key(Key, Pw);
decode_key(Key) ->
    decode_key(Key, no_passwd).
    

decode_key(#'RSAPublicKey'{} = Key,_) ->
    Key;
decode_key(#'RSAPrivateKey'{} = Key,_) ->
    Key;
decode_key(#'DSAPrivateKey'{} = Key,_) ->
    Key;
decode_key(PemEntry = {_,_,_}, Pw) ->
    public_key:pem_entry_decode(PemEntry, Pw);
decode_key(PemBin, Pw) ->
    [KeyInfo] = public_key:pem_decode(PemBin),
    decode_key(KeyInfo, Pw).

encode_key(Key = #'RSAPrivateKey'{}) ->
    {ok, Der} = 'OTP-PUB-KEY':encode('RSAPrivateKey', Key),
    {'RSAPrivateKey', list_to_binary(Der), not_encrypted};   
encode_key(Key = #'DSAPrivateKey'{}) ->
    {ok, Der} = 'OTP-PUB-KEY':encode('DSAPrivateKey', Key),
    {'DSAPrivateKey', list_to_binary(Der), not_encrypted}.

make_tbs(SubjectKey, Opts) ->    
    Version = list_to_atom("v"++integer_to_list(proplists:get_value(version, Opts, 3))),
    {Issuer, IssuerKey}  = issuer(Opts, SubjectKey),

    {Algo, Parameters} = sign_algorithm(IssuerKey, Opts),
    
    SignAlgo = #'SignatureAlgorithm'{algorithm  = Algo,
				     parameters = Parameters},    
    
    {#'OTPTBSCertificate'{serialNumber = trunc(random:uniform()*100000000)*10000 + 1,
			  signature    = SignAlgo,
			  issuer       = Issuer,
			  validity     = validity(Opts),
			  subject      = subject(proplists:get_value(subject, Opts),false),
			  subjectPublicKeyInfo = publickey(SubjectKey),
			  version      = Version,
			  extensions   = extensions(Opts)
			 }, IssuerKey}.

issuer(Opts, SubjectKey) ->
    IssuerProp = proplists:get_value(issuer, Opts, true),
    case IssuerProp of 
	true -> %% Self signed
	    {subject(proplists:get_value(subject, Opts), true), SubjectKey};
	{Issuer, IssuerKey} when is_binary(Issuer) ->
	    {issuer_der(Issuer), decode_key(IssuerKey)};
        {File, IssuerKey} when is_list(File) ->
	    {ok, [{cert, Cert, _}|_]} = public_key:pem_to_der(File),
	    {issuer_der(Cert), decode_key(IssuerKey)}
    end.

issuer_der(Issuer) ->
    Decoded = public_key:pkix_decode_cert(Issuer, otp),
    #'OTPCertificate'{tbsCertificate=Tbs} = Decoded,
    #'OTPTBSCertificate'{subject=Subject} = Tbs,
    Subject.

subject(undefined, IsCA) ->
    User = if IsCA -> "CA"; true -> os:getenv("USER") end,
    Opts = [{email, User ++ "@erlang.org"},
	    {name, User},
	    {city, "Stockholm"},
	    {country, "SE"},
	    {org, "erlang"},
	    {org_unit, "testing dep"}],
    subject(Opts);
subject(Opts, _) ->
    subject(Opts).

subject(SubjectOpts) when is_list(SubjectOpts) ->
    Encode = fun(Opt) ->
		     {Type,Value} = subject_enc(Opt),
		     [#'AttributeTypeAndValue'{type=Type, value=Value}]
	     end,
    {rdnSequence, [Encode(Opt) || Opt <- SubjectOpts]}.

%% Fill in the blanks
subject_enc({name,  Name}) ->       {?'id-at-commonName', {printableString, Name}};
subject_enc({email, Email}) ->      {?'id-emailAddress', Email};
subject_enc({city,  City}) ->       {?'id-at-localityName', {printableString, City}};
subject_enc({state, State}) ->      {?'id-at-stateOrProvinceName', {printableString, State}};
subject_enc({org, Org}) ->          {?'id-at-organizationName', {printableString, Org}};
subject_enc({org_unit, OrgUnit}) -> {?'id-at-organizationalUnitName', {printableString, OrgUnit}};
subject_enc({country, Country}) ->  {?'id-at-countryName', Country};
subject_enc({serial, Serial}) ->    {?'id-at-serialNumber', Serial};
subject_enc({title, Title}) ->      {?'id-at-title', {printableString, Title}};
subject_enc({dnQualifer, DnQ}) ->   {?'id-at-dnQualifier', DnQ};
subject_enc(Other) ->               Other.


extensions(Opts) ->
    case proplists:get_value(extensions, Opts, []) of
	false -> 
	    asn1_NOVALUE;
	Exts  -> 
	    lists:flatten([extension(Ext) || Ext <- default_extensions(Exts)])
    end.

default_extensions(Exts) ->
    Def = [{key_usage,undefined}, 
	   {subject_altname, undefined},
	   {issuer_altname, undefined},
	   {basic_constraints, default},
	   {name_constraints, undefined},
	   {policy_constraints, undefined},
	   {ext_key_usage, undefined},
	   {inhibit_any, undefined},
	   {auth_key_id, undefined},
	   {subject_key_id, undefined},
	   {policy_mapping, undefined}],
    Filter = fun({Key, _}, D) -> lists:keydelete(Key, 1, D) end,
    Exts ++ lists:foldl(Filter, Def, Exts).
       	
extension({_, undefined}) -> [];
extension({basic_constraints, Data}) ->
    case Data of
	default ->
	    #'Extension'{extnID = ?'id-ce-basicConstraints',
			 extnValue = #'BasicConstraints'{cA=true},
			 critical=true};
	false -> 
	    [];
	Len when is_integer(Len) ->
	    #'Extension'{extnID = ?'id-ce-basicConstraints',
			 extnValue = #'BasicConstraints'{cA=true, pathLenConstraint=Len},
			 critical=true};
	_ ->
	    #'Extension'{extnID = ?'id-ce-basicConstraints',
			 extnValue = Data}
    end;
extension({Id, Data, Critical}) ->
    #'Extension'{extnID = Id, extnValue = Data, critical = Critical}.


publickey(#'RSAPrivateKey'{modulus=N, publicExponent=E}) ->
    Public = #'RSAPublicKey'{modulus=N, publicExponent=E},
    Algo = #'PublicKeyAlgorithm'{algorithm= ?rsaEncryption, parameters='NULL'},
    #'OTPSubjectPublicKeyInfo'{algorithm = Algo,
			       subjectPublicKey = Public};
publickey(#'DSAPrivateKey'{p=P, q=Q, g=G, y=Y}) ->
    Algo = #'PublicKeyAlgorithm'{algorithm= ?'id-dsa', 
				 parameters=#'Dss-Parms'{p=P, q=Q, g=G}},
    #'OTPSubjectPublicKeyInfo'{algorithm = Algo, subjectPublicKey = Y}.

validity(Opts) ->
    DefFrom0 = date(),
    DefTo0   = calendar:gregorian_days_to_date(calendar:date_to_gregorian_days(date())+7),
    {DefFrom, DefTo} = proplists:get_value(validity, Opts, {DefFrom0, DefTo0}),
    Format = fun({Y,M,D}) -> lists:flatten(io_lib:format("~w~2..0w~2..0w000000Z",[Y,M,D])) end,
    #'Validity'{notBefore={generalTime, Format(DefFrom)},
		notAfter ={generalTime, Format(DefTo)}}.

sign_algorithm(#'RSAPrivateKey'{}, Opts) ->
    Type = case proplists:get_value(digest, Opts, sha1) of
	       sha1 ->   ?'sha1WithRSAEncryption';
	       sha512 -> ?'sha512WithRSAEncryption';
	       sha384 -> ?'sha384WithRSAEncryption';
	       sha256 -> ?'sha256WithRSAEncryption';
	       md5    -> ?'md5WithRSAEncryption';
	       md2    -> ?'md2WithRSAEncryption'
	   end,
    {Type, 'NULL'};
sign_algorithm(#'DSAPrivateKey'{p=P, q=Q, g=G}, _Opts) ->
    {?'id-dsa-with-sha1', #'Dss-Parms'{p=P, q=Q, g=G}}.

make_key(rsa, _Opts) ->
    %% (OBS: for testing only)
    gen_rsa2(64);
make_key(dsa, _Opts) ->
    gen_dsa2(128, 20).  %% Bytes i.e. {1024, 160} 
    
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%% RSA key generation  (OBS: for testing only)
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

-define(SMALL_PRIMES, [65537,97,89,83,79,73,71,67,61,59,53,
		       47,43,41,37,31,29,23,19,17,13,11,7,5,3]).

gen_rsa2(Size) ->
    P = prime(Size),
    Q = prime(Size),
    N = P*Q,
    Tot = (P - 1) * (Q - 1),
    [E|_] = lists:dropwhile(fun(Candidate) -> (Tot rem Candidate) == 0 end, ?SMALL_PRIMES),
    {D1,D2} = extended_gcd(E, Tot),
    D = erlang:max(D1,D2),
    case D < E of
	true ->
	    gen_rsa2(Size);
	false ->
	    {Co1,Co2} = extended_gcd(Q, P),
	    Co = erlang:max(Co1,Co2),
	    #'RSAPrivateKey'{version = 'two-prime',
			     modulus = N,
			     publicExponent  = E,
			     privateExponent = D, 
			     prime1 = P, 
			     prime2 = Q, 
			     exponent1 = D rem (P-1), 
			     exponent2 = D rem (Q-1), 
			     coefficient = Co
			    }
    end.

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%% DSA key generation  (OBS: for testing only)
%% See http://en.wikipedia.org/wiki/Digital_Signature_Algorithm
%% and the fips_186-3.pdf
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
gen_dsa2(LSize, NSize) ->
    Q  = prime(NSize),  %% Choose N-bit prime Q
    X0 = prime(LSize),
    P0 = prime((LSize div 2) +1),
    
    %% Choose L-bit prime modulus P such that p–1 is a multiple of q.
    case dsa_search(X0 div (2*Q*P0), P0, Q, 1000) of
	error -> 
	    gen_dsa2(LSize, NSize);
	P ->	    
	    G = crypto:mod_exp(2, (P-1) div Q, P), % Choose G a number whose multiplicative order modulo p is q.
	    %%                 such that This may be done by setting g = h^(p–1)/q mod p, commonly h=2 is used.
	    
	    X = prime(20),               %% Choose x by some random method, where 0 < x < q.
	    Y = crypto:mod_exp(G, X, P), %% Calculate y = g^x mod p.
	    
	    #'DSAPrivateKey'{version=0, p=P, q=Q, g=G, y=Y, x=X}
    end.
    
%% See fips_186-3.pdf
dsa_search(T, P0, Q, Iter) when Iter > 0 ->
    P = 2*T*Q*P0 + 1,
    case is_prime(crypto:mpint(P), 50) of
	true -> P;
	false -> dsa_search(T+1, P0, Q, Iter-1)
    end;
dsa_search(_,_,_,_) -> 
    error.


%%%%%%% Crypto Math %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
prime(ByteSize) ->
    Rand = odd_rand(ByteSize),
    crypto:erlint(prime_odd(Rand, 0)).

prime_odd(Rand, N) ->
    case is_prime(Rand, 50) of
	true -> 
	    Rand;
	false -> 
	    NotPrime = crypto:erlint(Rand),
	    prime_odd(crypto:mpint(NotPrime+2), N+1)
    end.

%% see http://en.wikipedia.org/wiki/Fermat_primality_test
is_prime(_, 0) -> true;
is_prime(Candidate, Test) -> 
    CoPrime = odd_rand(<<0,0,0,4, 10000:32>>, Candidate),
    case crypto:mod_exp(CoPrime, Candidate, Candidate) of
	CoPrime -> is_prime(Candidate, Test-1);
	_       -> false
    end.

odd_rand(Size) ->
    Min = 1 bsl (Size*8-1),
    Max = (1 bsl (Size*8))-1,
    odd_rand(crypto:mpint(Min), crypto:mpint(Max)).

odd_rand(Min,Max) ->
    Rand = <<Sz:32, _/binary>> = crypto:rand_uniform(Min,Max),
    BitSkip = (Sz+4)*8-1,
    case Rand of
	Odd  = <<_:BitSkip,  1:1>> -> Odd;
	Even = <<_:BitSkip,  0:1>> -> 
	    crypto:mpint(crypto:erlint(Even)+1)
    end.

extended_gcd(A, B) ->
    case A rem B of
	0 ->
	    {0, 1};
	N ->
	    {X, Y} = extended_gcd(B, N),
	    {Y, X-Y*(A div B)}
    end.