#include "os-interface.h"

#include <assert.h>
#include <hel-syscalls.h>
#include <hel.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include "library/cryptlib.h"

#define STUBBED { assert(!"unimplemented"); }

void libspdm_aead_free(void *context [[maybe_unused]]) {
}

#define SG_AEAD_AAD 0
#define SG_AEAD_TEXT 1
#define SG_AEAD_SIG 2
// Number of fields in AEAD scatterlist
#define SG_AEAD_LEN 3

int libspdm_aead(const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]], const uint8_t *iv [[maybe_unused]],
		size_t iv_size [[maybe_unused]], const uint8_t *a_data [[maybe_unused]], size_t a_data_size [[maybe_unused]],
		const uint8_t *data_in [[maybe_unused]], size_t data_in_size [[maybe_unused]], const uint8_t *tag [[maybe_unused]],
		size_t tag_size [[maybe_unused]], uint8_t *data_out [[maybe_unused]], size_t *data_out_size [[maybe_unused]],
		bool enc [[maybe_unused]], char const *alg [[maybe_unused]]) {
	return -ENODEV;
}

// Wrapper to make look like libspdm
bool libspdm_aead_gcm_prealloc(void **context [[maybe_unused]]) {
	return false;
}

bool libspdm_aead_aes_gcm_encrypt_prealloc(void *context [[maybe_unused]],
		const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
		const uint8_t *iv [[maybe_unused]], size_t iv_size [[maybe_unused]],
		const uint8_t *a_data [[maybe_unused]], size_t a_data_size [[maybe_unused]],
		const uint8_t *data_in [[maybe_unused]], size_t data_in_size [[maybe_unused]],
		uint8_t *tag_out [[maybe_unused]], size_t tag_size [[maybe_unused]],
		uint8_t *data_out [[maybe_unused]], size_t *data_out_size [[maybe_unused]]) {
	return false;
}

bool libspdm_aead_aes_gcm_decrypt_prealloc(void *context [[maybe_unused]],
		const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
		const uint8_t *iv [[maybe_unused]], size_t iv_size [[maybe_unused]],
		const uint8_t *a_data [[maybe_unused]], size_t a_data_size [[maybe_unused]],
		const uint8_t *data_in [[maybe_unused]], size_t data_in_size [[maybe_unused]],
		const uint8_t *tag [[maybe_unused]], size_t tag_size [[maybe_unused]],
		uint8_t *data_out [[maybe_unused]], size_t *data_out_size [[maybe_unused]]) {
	return false;
}

void *libspdm_hmac_sha256_new(void) {
	return NULL;
}

void libspdm_hmac_sha256_free(void *hmac_sha256_ctx [[maybe_unused]]) {

}

bool libspdm_hmac_sha256_set_key(void *hmac_sha256_ctx [[maybe_unused]], const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]]) {
	return false;
}

bool libspdm_hmac_sha256_duplicate(const void *hmac_sha256_ctx [[maybe_unused]], void *new_hmac_sha256_ctx [[maybe_unused]]) {
	return false;
}

bool libspdm_hmac_sha256_update(void *hmac_sha256_ctx [[maybe_unused]], const void *data [[maybe_unused]], size_t data_size [[maybe_unused]]) {
	return false;
}

bool libspdm_hmac_sha256_final(void *hmac_sha256_ctx [[maybe_unused]], uint8_t *hmac_value [[maybe_unused]]) {
	return false;
}

bool libspdm_hmac_sha256_all(const void *data [[maybe_unused]], size_t data_size [[maybe_unused]],
	const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
	uint8_t *hmac_value [[maybe_unused]]) {
	return false;
}

bool libspdm_check_crypto_backend(void) {
	nv_printf(NV_DBG_ERRORS, "libspdm_check_crypto_backend: Error - libspdm expects LKCA but found stubs!\n");
	return false;
}

bool libspdm_x509_verify_cert_chain(const uint8_t *root_cert [[maybe_unused]], size_t root_cert_length [[maybe_unused]],
	const uint8_t *cert_chain [[maybe_unused]], size_t cert_chain_length [[maybe_unused]]) STUBBED;

bool libspdm_x509_get_cert_from_cert_chain(const uint8_t *cert_chain [[maybe_unused]],
	size_t cert_chain_length [[maybe_unused]],
	const int32_t cert_index [[maybe_unused]], const uint8_t **cert [[maybe_unused]],
	size_t *cert_length [[maybe_unused]]) STUBBED;

bool libspdm_rsa_get_public_key_from_x509(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		void **rsa_context [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_ec_get_public_key_from_x509(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		void **ec_context [[maybe_unused]]) {
	return false;
}

bool libspdm_x509_verify_cert(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
	const uint8_t *ca_cert [[maybe_unused]], size_t ca_cert_size [[maybe_unused]]) STUBBED;

bool libspdm_rsa_pss_sign(void *rsa_context [[maybe_unused]], size_t hash_nid [[maybe_unused]],
	const uint8_t *message_hash [[maybe_unused]], size_t hash_size [[maybe_unused]],
	uint8_t *signature [[maybe_unused]], size_t *sig_size [[maybe_unused]]) STUBBED;

bool libspdm_ecdsa_sign(void *ec_context [[maybe_unused]], size_t hash_nid [[maybe_unused]],
	const uint8_t *message_hash [[maybe_unused]], size_t hash_size [[maybe_unused]],
	uint8_t *signature [[maybe_unused]], size_t *sig_size [[maybe_unused]]) STUBBED;

void libspdm_rsa_free(void *rsa_context [[maybe_unused]]) {}
void libspdm_ec_free(void *ec_context [[maybe_unused]]) {}

bool libspdm_rsa_pss_verify(void *rsa_context [[maybe_unused]], size_t hash_nid [[maybe_unused]],
	const uint8_t *message_hash [[maybe_unused]], size_t hash_size [[maybe_unused]],
	const uint8_t *signature [[maybe_unused]], size_t sig_size [[maybe_unused]]) {
	return false;
}

bool libspdm_ecdsa_verify(void *ec_context [[maybe_unused]], size_t hash_nid [[maybe_unused]],
	const uint8_t *message_hash [[maybe_unused]], size_t hash_size [[maybe_unused]],
	const uint8_t *signature [[maybe_unused]], size_t sig_size [[maybe_unused]]) STUBBED;

bool libspdm_asn1_get_tag(uint8_t **ptr [[maybe_unused]], const uint8_t *end [[maybe_unused]], size_t *length [[maybe_unused]],
	uint32_t tag [[maybe_unused]]) STUBBED;


bool libspdm_x509_get_tbs_cert(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		uint8_t **tbs_cert [[maybe_unused]], size_t *tbs_cert_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_version(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		size_t *version [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_serial_number(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		uint8_t *serial_number [[maybe_unused]], size_t *serial_number_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_issuer_name(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		uint8_t *cert_issuer [[maybe_unused]], size_t *issuer_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_issuer_common_name(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		char *common_name [[maybe_unused]], size_t *common_name_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_issuer_orgnization_name(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		char *name_buffer [[maybe_unused]], size_t *name_buffer_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_signature_algorithm(const uint8_t *cert [[maybe_unused]],
		size_t cert_size [[maybe_unused]], uint8_t *oid [[maybe_unused]], size_t *oid_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_extension_data(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		const uint8_t *oid [[maybe_unused]], size_t oid_size [[maybe_unused]],
		uint8_t *extension_data [[maybe_unused]], size_t *extension_data_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_validity(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		uint8_t *from [[maybe_unused]], size_t *from_size [[maybe_unused]], uint8_t *to [[maybe_unused]],
		size_t *to_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_key_usage(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		size_t *usage [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_extended_key_usage(const uint8_t *cert [[maybe_unused]],
		size_t cert_size [[maybe_unused]], uint8_t *usage [[maybe_unused]],
		size_t *usage_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_extended_basic_constraints(const uint8_t *cert [[maybe_unused]],
		size_t cert_size [[maybe_unused]], uint8_t *basic_constraints [[maybe_unused]],
		size_t *basic_constraints_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_set_date_time(char const *date_time_str [[maybe_unused]], void *date_time [[maybe_unused]], size_t *date_time_size [[maybe_unused]]) {
	assert(false);
	return false;
}

int32_t libspdm_x509_compare_date_time(const void *date_time1 [[maybe_unused]], const void *date_time2 [[maybe_unused]]) {
	assert(false);
	return -3;
}

bool libspdm_gen_x509_csr(size_t hash_nid [[maybe_unused]], size_t asym_nid [[maybe_unused]],
		uint8_t *requester_info [[maybe_unused]], size_t requester_info_length [[maybe_unused]],
		void *context [[maybe_unused]], char *subject_name [[maybe_unused]],
		size_t *csr_len [[maybe_unused]], uint8_t **csr_pointer [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_subject_name(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		uint8_t *cert_subject [[maybe_unused]],
		size_t *subject_size [[maybe_unused]]) {
	assert(false);
	return false;
}

bool libspdm_x509_get_common_name(const uint8_t *cert [[maybe_unused]], size_t cert_size [[maybe_unused]],
		char *common_name [[maybe_unused]],
		size_t *common_name_size [[maybe_unused]]) {
	assert(false);
	return false;
}

void *libspdm_sha256_new(void) {
	return NULL;
}

void libspdm_sha256_free(void *sha256_ctx [[maybe_unused]]) {}

bool libspdm_sha256_init(void *sha256_context [[maybe_unused]]) STUBBED;
bool libspdm_sha256_duplicate(const void *sha256_context [[maybe_unused]],
	void *new_sha256_context [[maybe_unused]]) {
	return false;
}
bool libspdm_sha256_update(void *sha256_context [[maybe_unused]], const void *data [[maybe_unused]],
	size_t data_size [[maybe_unused]]) STUBBED;
bool libspdm_sha256_final(void *sha256_context [[maybe_unused]], uint8_t *hash_value [[maybe_unused]]) STUBBED;

void *libspdm_sha384_new(void) {
	return NULL;
}

void libspdm_sha384_free(void *sha384_ctx [[maybe_unused]]) {}

bool libspdm_sha384_init(void *sha384_context [[maybe_unused]]) STUBBED;
bool libspdm_sha384_duplicate(const void *sha384_context [[maybe_unused]],
	void *new_sha384_context [[maybe_unused]]) {
	return false;
}
bool libspdm_sha384_update(void *sha384_context [[maybe_unused]], const void *data [[maybe_unused]],
	size_t data_size [[maybe_unused]]) STUBBED;
bool libspdm_sha384_final(void *sha384_context [[maybe_unused]], uint8_t *hash_value [[maybe_unused]]) STUBBED;

bool libspdm_sha256_hash_all(const void *data [[maybe_unused]], size_t data_size [[maybe_unused]],
	uint8_t *hash_value [[maybe_unused]]) STUBBED;

bool libspdm_sha384_hash_all(const void *data [[maybe_unused]], size_t data_size [[maybe_unused]],
	uint8_t *hash_value [[maybe_unused]]) STUBBED;

void *libspdm_rsa_new(void) {
	return NULL;
}

bool libspdm_rsa_set_key(void *rsa_context [[maybe_unused]], const libspdm_rsa_key_tag_t  key_tag [[maybe_unused]],
		const uint8_t *big_number [[maybe_unused]], size_t bn_size [[maybe_unused]]) {
	return false;
}

bool libspdm_hkdf_sha256_expand(const uint8_t *prk [[maybe_unused]], size_t prk_size [[maybe_unused]],
	const uint8_t *info [[maybe_unused]], size_t info_size [[maybe_unused]],
	uint8_t *out [[maybe_unused]], size_t out_size [[maybe_unused]]) STUBBED;

bool libspdm_aead_aes_gcm_encrypt(const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
	const uint8_t *iv [[maybe_unused]], size_t iv_size [[maybe_unused]],
	const uint8_t *a_data [[maybe_unused]], size_t a_data_size [[maybe_unused]],
	const uint8_t *data_in [[maybe_unused]], size_t data_in_size [[maybe_unused]],
	uint8_t *tag_out [[maybe_unused]], size_t tag_size [[maybe_unused]],
	uint8_t *data_out [[maybe_unused]], size_t *data_out_size [[maybe_unused]]) STUBBED;

bool libspdm_aead_aes_gcm_decrypt(const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
	const uint8_t *iv [[maybe_unused]], size_t iv_size [[maybe_unused]],
	const uint8_t *a_data [[maybe_unused]], size_t a_data_size [[maybe_unused]],
	const uint8_t *data_in [[maybe_unused]], size_t data_in_size [[maybe_unused]],
	const uint8_t *tag [[maybe_unused]], size_t tag_size [[maybe_unused]],
	uint8_t *data_out [[maybe_unused]], size_t *data_out_size [[maybe_unused]]) STUBBED;

void *libspdm_ec_new_by_nid(size_t nid [[maybe_unused]]) STUBBED;

bool libspdm_ec_generate_key(void *ec_context [[maybe_unused]], uint8_t *public_data [[maybe_unused]],
	size_t *public_size [[maybe_unused]]) STUBBED;

bool libspdm_ec_compute_key(void *ec_context [[maybe_unused]], const uint8_t *peer_public [[maybe_unused]],
	size_t peer_public_size [[maybe_unused]], uint8_t *key [[maybe_unused]],
	size_t *key_size [[maybe_unused]]) STUBBED;

bool libspdm_hkdf_sha256_extract(const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
	const uint8_t *salt [[maybe_unused]], size_t salt_size [[maybe_unused]],
	uint8_t *prk_out [[maybe_unused]], size_t prk_out_size [[maybe_unused]]) STUBBED;


bool libspdm_hkdf_sha384_extract(const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
	const uint8_t *salt [[maybe_unused]], size_t salt_size [[maybe_unused]],
	uint8_t *prk_out [[maybe_unused]], size_t prk_out_size [[maybe_unused]]) STUBBED;

bool libspdm_hkdf_sha384_expand(const uint8_t *prk [[maybe_unused]], size_t prk_size [[maybe_unused]],
	const uint8_t *info [[maybe_unused]], size_t info_size [[maybe_unused]],
	uint8_t *out [[maybe_unused]], size_t out_size [[maybe_unused]]) STUBBED;

void *libspdm_hmac_sha384_new() STUBBED;
void libspdm_hmac_sha384_free(void *hmac_sha384_ctx [[maybe_unused]] [[maybe_unused]]) STUBBED;
bool libspdm_hmac_sha384_set_key(void *hmac_sha384_ctx [[maybe_unused]], const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]]) STUBBED;
bool libspdm_hmac_sha384_duplicate(const void *hmac_sha384_ctx [[maybe_unused]], void *new_hmac_sha384_ctx [[maybe_unused]] [[maybe_unused]]) STUBBED;
bool libspdm_hmac_sha384_update(void *hmac_sha384_ctx [[maybe_unused]], const void *data [[maybe_unused]], size_t data_size [[maybe_unused]]) STUBBED;
bool libspdm_hmac_sha384_final(void *hmac_sha384_ctx [[maybe_unused]], uint8_t *hmac_value [[maybe_unused]]) STUBBED;
bool libspdm_hmac_sha384_all(const void *data [[maybe_unused]], size_t data_size [[maybe_unused]],
	const uint8_t *key [[maybe_unused]], size_t key_size [[maybe_unused]],
	uint8_t *hmac_value [[maybe_unused]]) STUBBED;

bool libspdm_random_bytes(uint8_t *output, size_t size) {
	size_t read = 0;

	while(read < size) {
		size_t actualSize = 0;
		helGetRandomBytes(output + read, size - read, &actualSize);
		read += actualSize;
	}

	return true;
}

// This is specifically allowed by spdm
bool libspdm_random_seed(const uint8_t *seed [[maybe_unused]], size_t seed_size [[maybe_unused]]) {
	return true;
}
