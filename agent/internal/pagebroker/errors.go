// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package pagebroker

import (
	"errors"
	"fmt"
)

var errMessageTooLarge = fmt.Errorf("message exceeds %d bytes", maxMessageSize)

// transportError wraps a connection or I/O failure, as opposed to one
// PageBroker itself reported. A dial failure is marked separately: unlike
// a mid-exchange failure, it never reached PageBroker.
type transportError struct {
	cause error
	dial  bool
}

func (e transportError) Error() string { return e.cause.Error() }

func (e transportError) Unwrap() error { return e.cause }

func isTransportError(err error) bool {
	var transport transportError
	return errors.As(err, &transport)
}

// isRetryableTransportError reports a transport failure Commit should retry: an
// exchange whose outcome is unknown because the connection broke mid-request. A
// dial failure is never retryable here; dial already retried the transient
// cases itself, so what reaches this check is either a permanent error or a
// transient one that exhausted its own budget.
func isRetryableTransportError(err error) bool {
	return isTransportError(err) && !IsDialError(err)
}

// IsDialError reports whether err came from failing to connect to PageBroker,
// in which case the request never reached it.
func IsDialError(err error) bool {
	var transport transportError
	return errors.As(err, &transport) && transport.dial
}

// failureError carries a Failure reply from PageBroker.
type failureError struct {
	code    Failure_Code
	message string
}

func (e failureError) Error() string {
	return fmt.Sprintf("PageBroker %s: %s", e.code, e.message)
}

func failureCode(code Failure_Code) Failure_Code {
	switch code {
	case Failure_UNSPECIFIED, Failure_INVALID_REQUEST, Failure_TRANSACTION_NOT_FOUND, Failure_TRANSACTION_CONFLICT,
		Failure_INSUFFICIENT_STORAGE, Failure_STORAGE_ERROR, Failure_INTERNAL_ERROR, Failure_UNAVAILABLE:
		return code
	default:
		return Failure_UNSPECIFIED
	}
}
