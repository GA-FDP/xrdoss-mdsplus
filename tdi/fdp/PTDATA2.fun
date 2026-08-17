/**************************************************************************************************************************
        PUBLIC FUN PTDATA2(IN _pointname, OPTIONAL IN _shot, OPTIONAL IN _ical,
                           OPTIONAL OUT _error, OPTIONAL IN _double)

        Time-history DIII-D data from PTDATA, over ptdata's C ABI.

        The signature is positional and stored tree records call it
        positionally, so it is preserved exactly -- including _double, which is
        accepted and ignored: the ABI returns doubles unconditionally, so there
        is nothing left to toggle.

        Everything the legacy version did in TDI -- segmented-DFI dispatch, the
        PCS timebase special cases, the per-DFI ical arithmetic, roughly 250
        lines of it -- the C++ reader does internally. That is the point of the
        rewrite: it was untestable here and is unit-tested there. The fork has
        already had to fix a bug in that TDI arithmetic ("missing || operators
        in ptdata2.fun IF condition"), which is still present in the copy the
        mdsplus-d3d RPM ships.

        ical: 0 raw counts, 1 physics units (default), 2 volts into the
        digitizer, 4 integrated v-sec. Anything else RAISES rather than
        silently substituting a calibration -- wrong numbers that look right
        are the worst available outcome. Measured across ~1,700 tree-embedded
        calls spanning 14 years, the set actually used is {1, 2, 4}.

**************************************************************************************************************************/

PUBLIC FUN PTDATA2(IN _pointname, OPTIONAL IN _shot, OPTIONAL IN _ical,
                   OPTIONAL OUT _error, OPTIONAL IN _double) {

	IF (NOT PRESENT(_shot)) _shot = $SHOT;
	IF (NOT PRESENT(_ical)) _ical = 1;

	_npts = 0; _ntimes = 0;
	_error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_size",
	                    _pointname, REF(LONG(_shot)), REF(LONG(_ical)),
	                    REF(_npts), REF(_ntimes));

	/* 1 = PointnameNotFound, 3 = ShotNotFound: absent data. [0] is what
	   PTDATA2 callers already expect and what production returns.
	   Everything else -- 110 unsupported ical, 200/201 our own bugs -- is a
	   real failure. The legacy function collapsed both into [0]; owning
	   this wrapper is what lets a genuine failure be a genuine failure. */
	IF (_error == 1 || _error == 3) { RETURN([0]); }
	IF (_error != 0) { ABORT(); }
	IF (_npts <= 0) { RETURN([0]); }

	/* _ntimes+1 rather than MAX(_ntimes,1): ZERO(0,...) is not a usable
	   buffer, and only _ntimes elements are written regardless of how many
	   are allocated. */
	_data  = ZERO(_npts, 0D0);
	_times = ZERO(_ntimes + 1, 0D0);
	_error = BUILD_CALL(8, PTD3D_LIBRARY(), "ptdata_capi_copy",
	                    _pointname, REF(LONG(_shot)), REF(LONG(_ical)),
	                    REF(_npts), REF(_ntimes), REF(_data), REF(_times));
	IF (_error != 0) { ABORT(); }

	/* The header, for the globals below and for the time fallback. A
	   separate error variable: PTHEAD2's OUT parameter would otherwise
	   overwrite ours and report a header status as this call's status.
	   Costs no extra fetch -- the C ABI retains the parsed header under its
	   own (pointname, shot) key, independent of the data result. */
	_herr = 0;
	_ignored = PTHEAD2(_pointname, _shot, _herr);

	/* Units. The engine's times are already MILLISECONDS. The header's
	   rarray[7] (start) and rarray[8] (delta) are SECONDS, which is why the
	   fallback scales and this branch does not. Getting that backwards is a
	   1000x error that still looks like a plausible time axis. */
	IF (_ntimes > 0) {
		_time = _times[0 .. _ntimes - 1];
	} ELSE {
		/* A DFI with no dedicated handler is served by GenericDfi, which
		   produces no time base. The legacy function had the same
		   fallback: synthesize from the header's start and delta. */
		_time = (RAMP(_npts) * PUBLIC __rarray[8] + PUBLIC __rarray[7]) * 1000D0;
	}

	/* Set by the legacy version for a handful of CAMAC DFIs, from
	   __int16[6..8] against the same two-control-word layout.

	   No DFI-list guard here. Measured 2026-08-16: no site TDI function
	   reads __branch, __crate or __slot at all -- only a stored record
	   could, and that scan has not been run. Setting them unconditionally
	   is three assignments and cannot be wrong; reproducing a DFI list we
	   have no consumer for would be dead code.

	   Deliberately NOT set: __ptdata_pointname / __ptdata_shot. Those are
	   ptdata_historic.fun's memo keys, and writing them would make its
	   cache return our signal for a different point. */
	PUBLIC __branch = 0;
	PUBLIC __crate  = 0;
	PUBLIC __slot   = 0;
	IF (SIZE(PUBLIC __int16) >= 9) {
		PUBLIC __branch = PUBLIC __int16[6];
		PUBLIC __crate  = PUBLIC __int16[7];
		PUBLIC __slot   = PUBLIC __int16[8];
	}

	PUBLIC __ptdata_signal =
		MAKE_SIGNAL(_data, *, MAKE_DIM(*, MAKE_WITH_UNITS(_time, "ms")));
	RETURN(PUBLIC __ptdata_signal);
}
