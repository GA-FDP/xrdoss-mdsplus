/**************************************************************************************************************************
        PUBLIC FUN PTHEAD2(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error)

        The legacy PTDATA header call, over ptdata's C ABI instead of libd3's
        ptdata64_.

        The return value is the SMALLER half of the contract. Six one-line
        shims read this function's side effects rather than its result:

            PTHEAD_IFIX    return(_ifix)          <- the return value
            PTHEAD_RFIX    return(__rarray)
            PTHEAD_REAL32  return(__real32[2:*])
            PTHEAD_INT16   return(__int16[2:*])
            PTHEAD_INT32   return(__int32[2:*])
            PTHEAD_ASCII   via PTHEAD2_ASCII, which reads __ascii

        548 of the 596 measured PTHEAD2 calls arrive through those, so the
        seven PUBLIC globals ARE the interface. Dropping one breaks callers
        that never look at what this function returns.

        Each variable-header section is [n, n, values...]: two control words
        then the data. That is why PTHEAD_REAL32 skips [2:*], and why an empty
        section has length 2 rather than 0 -- PTHEAD_REAL32 then ABORTs, which
        is what the legacy did too and must not be "fixed".

        ptdata's legacy_header::build produces that layout; this function only
        transports it.

**************************************************************************************************************************/

PUBLIC FUN PTHEAD2(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error) {

	IF (NOT PRESENT(_shot)) _shot = $SHOT;

	_ni = 0; _nr = 0; _na = 0; _n16 = 0; _n32 = 0; _nf32 = 0; _nf64 = 0;

	/* rtype 8 is DTYPE_L: BUILD_CALL(8, ...) delivers the callee's int
	   return. rtype 0 (DTYPE_MISSING) DISCARDS it, which is why every
	   legacy .fun had to communicate through a REF(_ier) out-parameter
	   instead. Measured, not assumed -- see the plan's Task 0 findings. */
	_error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_header_size",
	                    _pointname, REF(LONG(_shot)),
	                    REF(_ni), REF(_nr), REF(_na),
	                    REF(_n16), REF(_n32), REF(_nf32), REF(_nf64));

	/* 1 = PointnameNotFound, 3 = ShotNotFound. Those are absent data, not a
	   failure to get it, and callers already expect zeros. Everything else
	   is a real error -- an unreadable shotfile, a malformed header -- and
	   must not be smoothed into a zeroed header that reads as valid. */
	IF (_error == 1 || _error == 3) {
		PUBLIC __iarray = ZERO(50, 0);
		PUBLIC __rarray = ZERO(20, 0.0);
		PUBLIC __ascii  = [0, 0];
		PUBLIC __int16  = ZERO(2, 0W);
		PUBLIC __int32  = [0, 0];
		PUBLIC __real32 = [0.0, 0.0];
		PUBLIC __real64 = [0D0, 0D0];
		RETURN(PUBLIC __iarray);
	}
	IF (_error != 0) { ABORT(); }

	/* The literal in each ZERO() picks the element type, and it must match
	   what the C ABI writes or the copy overruns half the buffer:
	     0    int32 (iarray, ascii, int32)      0W  int16
	     0.0  float32 (rarray, real32)          0D0 float64
	   Measured with ZERO(4,0W).dtype == int16; ZERO(4,0) is int32. */
	_iarray = ZERO(_ni,   0);
	_rarray = ZERO(_nr,   0.0);
	_ascii  = ZERO(_na,   0);
	_int16  = ZERO(_n16,  0W);
	_int32  = ZERO(_n32,  0);
	_real32 = ZERO(_nf32, 0.0);
	_real64 = ZERO(_nf64, 0D0);

	_error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_header_copy",
	                    _pointname, REF(LONG(_shot)),
	                    REF(_ni),   REF(_iarray),
	                    REF(_nr),   REF(_rarray),
	                    REF(_na),   REF(_ascii),
	                    REF(_n16),  REF(_int16),
	                    REF(_n32),  REF(_int32),
	                    REF(_nf32), REF(_real32),
	                    REF(_nf64), REF(_real64));
	IF (_error != 0) { ABORT(); }

	PUBLIC __iarray = _iarray;
	PUBLIC __rarray = _rarray;
	PUBLIC __ascii  = _ascii;
	PUBLIC __int16  = _int16;
	PUBLIC __int32  = _int32;
	PUBLIC __real32 = _real32;
	PUBLIC __real64 = _real64;

	RETURN(_iarray);
}
