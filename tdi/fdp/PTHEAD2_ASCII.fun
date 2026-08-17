/**************************************************************************************************************************
        PUBLIC FUN PTHEAD2_ASCII(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error)

        The ASCII variable-header section, as text.

        __ascii is [n, n, words...] where each word packs 4 characters
        little-endian, so the decode starts 8 bytes in -- exactly what the
        legacy version's `_idx + 8.` does, and the same two control words
        PTHEAD_REAL32 skips with [2:*].

        Padding bytes are dropped. The legacy dropped both NUL and 12800
        (0x3200, a byte-order artefact of the VAX-era packing); keeping both
        tests matters because PCS DFIs 2201/2202/2203 store a CLOCK POINTNAME
        here and then look it up. Appending junk to it turns a working lookup
        into a not-found.

        The legacy called PTHEAD2_SIZE first to learn the length, then made a
        second ptdata_ call to fill the buffer. Our PTHEAD2 already produced
        __ascii, so only the decode remains.

**************************************************************************************************************************/

PUBLIC FUN PTHEAD2_ASCII(IN _pointname, OPTIONAL IN _shot, OPTIONAL OUT _error) {

	PRIVATE FUN c(IN _i, IN _idx) {
		_j = _idx + 8;
		_ichr = _i[_j / 4] >> ((_j MOD 4) * 8);
		IF ((_ichr == 0) || (_ichr == 12800)) { RETURN("NULL"); }
		RETURN(CHAR(_ichr & 0xff));
	};

	IF (NOT PRESENT(_shot)) _shot = $SHOT;

	_error = 0;
	_ignored = PTHEAD2(_pointname, _shot, _error);
	IF (_error != 0) { RETURN(""); }

	_ascii = PUBLIC __ascii;

	/* Length 2 is an EMPTY section, not a one-word one: the two leading
	   control words are always present. Reading past them here would decode
	   whatever follows in memory. */
	IF (SIZE(_ascii) <= 2) { RETURN(""); }

	_text = "";
	_sec = 4 * _ascii[1];
	FOR (_i = 0; _i < _sec; _i++) {
		_achr = c(_ascii, _i);
		IF (_achr != "NULL") { _text = _text // _achr; }
	}
	RETURN(_text);
}
