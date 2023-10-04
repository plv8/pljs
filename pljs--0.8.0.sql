CREATE OR REPLACE FUNCTION pljs_call_handler() RETURNS language_handler
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE FUNCTION pljs_call_validator(oid) RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE FUNCTION pljs_inline_handler(internal) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE TRUSTED LANGUAGE pljs
 HANDLER pljs_call_handler
 INLINE pljs_inline_handler
 VALIDATOR pljs_call_validator;
