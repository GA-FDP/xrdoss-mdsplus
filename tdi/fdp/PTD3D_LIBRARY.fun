/**************************************************************************************************************************
        PUBLIC FUN PTD3D_LIBRARY()

        Where libptd3d.so lives.

        One place, because three functions BUILD_CALL into it and a wrong path
        fails all three the same silent way -- BUILD_CALL on a library that
        cannot be loaded is not a loud error.

        Mirrors the legacy PTDATA_LIBRARY(), which reads $PTDATA_LIBRARY,
        except that this one has a default. The sandbox image installs to a
        path we choose, so requiring the environment variable would add a way
        to misconfigure the deployment with no corresponding benefit. The
        override stays for development, where the library is wherever the
        build put it.

        Measured 2026-08-16, because two plausible guesses were both wrong:

          * TranslateLogical on an UNSET name returns "", not $MISSING and not
            an error. So LEN()==0 is the test, and PRESENT() would never fire.
          * TranslateLogical is not a builtin. It is tdi/mdsshr/TranslateLogical.fun,
            so /usr/local/mdsplus/tdi/mdsshr must be on MDS_PATH or the call
            raises %TDI-E-UNKNOWN_VAR -- which reads exactly like an unset
            logical and is not one. The site library needs that directory
            anyway (ptdata2.fun calls translatelogical("VENDOR")).

        The IF_ERROR covers the second case so that a missing tdi/mdsshr
        degrades to the default path rather than taking down every PTDATA2
        call with it.

**************************************************************************************************************************/

PUBLIC FUN PTD3D_LIBRARY() {

	_lib = IF_ERROR(TranslateLogical("PTD3D_LIBRARY"), "");

	IF (LEN(_lib) == 0) { RETURN("/usr/local/ptdata/lib/libptd3d.so"); }

	RETURN(_lib);
}
