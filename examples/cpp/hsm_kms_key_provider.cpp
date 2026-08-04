#include "dage/bundle_resolver.hpp"

#include <functional>
#include <utility>

// Adapt a PKCS#11, cloud KMS, or organization key-directory client here. DAGE only
// needs the Ed25519 public key; private signing material never enters the runtime.
class HsmKmsKeyProvider final : public dage::KeyProvider {
public:
    using Lookup = std::function<dage::Result<dage::PublicKey>(
        const std::string&, const dage::SignatureContext&)>;

    explicit HsmKmsKeyProvider(Lookup lookup) : lookup_(std::move(lookup)) {}

    dage::Result<dage::PublicKey> find_key(
        const std::string& key_id,
        const dage::SignatureContext& context) const override {
        auto result = lookup_(key_id, context);
        if (!result || result.value().bytes.size() == 32) return result;
        dage::Error error;
        error.category = "trust";
        error.code = "INVALID_ED25519_PUBLIC_KEY";
        error.message = "external key service returned a non-Ed25519 public key";
        return dage::Result<dage::PublicKey>::failure(std::move(error));
    }

private:
    Lookup lookup_;
};

int main() {
    HsmKmsKeyProvider provider(
        [](const std::string&, const dage::SignatureContext&) {
            // Replace with a pinned, authenticated SDK call or PKCS#11 lookup.
            // Cache only according to the host's rotation/revocation SLA.
            dage::Error error;
            error.category = "trust";
            error.code = "EXTERNAL_KEY_NOT_CONFIGURED";
            error.message = "configure the host HSM/KMS adapter";
            return dage::Result<dage::PublicKey>::failure(std::move(error));
        });
    (void)provider;
    return 0;
}
