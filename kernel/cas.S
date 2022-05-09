.global cas
    # a0 holds address of memory location
    # a1 holds expected value
    # a2 holds desired value
    # a0 holds return value, 0 if successful, !0 otherwise
cas:
    lr.w t0, (a0)       # Load original value.
    bne t0, a1, fail    # Doesn't match, so fail.
    sc.w a0, a2, (a0)   # Try to update.
    # li a0, 0            # Return value is a0.
    jr ra               # Return 0, i.e., FALSE for sucess.
fail:
    li a0, 1            # Set return to failure. --> Returns 1 for failure (i.e., TRUE).
    jr ra               # Return.
