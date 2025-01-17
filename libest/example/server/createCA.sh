#!/usr/bin/env bash

# default variables and config options
. ESTcommon.sh

# completely clean out and re-create demoCA database directories (DESTRUCTIVE!!)
echo "#################################################################"
echo "SECURITY CONSIDERATIONS - NOTE WELL"
echo "The sample scripts used to handle EST operations are NOT"
echo "intended to provide a secure implementation. They have not"
echo "been evaluated for security, they have not had a Thread Model"
echo "reviewed, they are not particularly good about cleaning up after"
echo "themselves and they assume the data exchanged is well formed"
echo "if the cryptographic checks pass."
echo ""
echo "In short: They are not to be trusted. They provide a functional"
echo "implementation only."
echo ""
echo "Continuing will completely erase/destroy/nuke the existing estCA"
echo "Press any key..."
read -n 1
echo ""
echo "Nuking from orbit!"
echo "#################################################################"
rm -rf $EST_OPENSSL_CADIR
rm -rf $EST_OPENSSL_EXTCADIR

# given a base directory name this sets up a full CA for use
function createCA () 
{
    # inputs
    CREATECA_CASUBJ=$1
    CREATECA_CADIR=$2
    CREATECA_CACERT=$3
    CREATECA_CAPRIVDIR=$4
    CREATECA_CASERIAL=$5
    CREATECA_CADATABASE=$6
    CREATECA_CANEWCERTSDIR=$7
    CREATECA_CAPRIVKEY=$8
    CREATECA_NEWKEY_PARAM=$9
    CREATECA_ECPARAMSFILE=${10}
    # ISARA: BEGIN
    CREATECA_CAPRIVKEY2=${11}
    # ISARA: END

    echo "#################################################################"
    echo "####(Re)creating the directory structure and initial files" 
    echo "####for the CA: $CREATECA_CASUBJ"
    echo "#################################################################"
    mkdir $CREATECA_CADIR
    mkdir $CREATECA_CAPRIVDIR
    mkdir $CREATECA_CANEWCERTSDIR
    echo "01" > $CREATECA_CASERIAL
    touch $CREATECA_CADATABASE
    
    # This is only needed for EC mode
    eval $OPENSSLCMD ecparam -name prime256v1 -out $CREATECA_ECPARAMSFILE
    iferrorlogandexit "Unable to build ECPARAMS file" 1

    eval $OPENSSLCMD req -new -x509 -sha256 -extensions v3_ca $CREATECA_NEWKEY_PARAM -keyout $CREATECA_CAPRIVKEY -out $CREATECA_CACERT -days 365 -nodes -subj "$CREATECA_CASUBJ" -config $EST_OPENSSL_CACNF
    iferrorlogandexit "Unable to create \"$CREATECA_CASUBJ\" CA cert" 1
    # ISARA: BEGIN
    # generate alternative key to use in multiple public-key algorithm certificate
    $OPENSSLCMD genpkey -engine qs_sig -algorithm hss -pkeyopt winternitz_value:8 -pkeyopt tree_height:10 -out $CREATECA_CAPRIVKEY2
    iferrorlogandexit "Unable to create \"$CREATECA_CASUBJ\" second key" 1
    mv $CREATECA_CACERT "${CREATECA_CACERT}.bak"
    # Convert certificate to multiple public-key algorithm certificate
    $OPENSSLCMD x509QSDirectExtend -engine qs_sig -x509in "${CREATECA_CACERT}.bak" -x509out $CREATECA_CACERT -privin $CREATECA_CAPRIVKEY -self_sign -privqs $CREATECA_CAPRIVKEY2
    iferrorlogandexit "Unable to create \"$CREATECA_CASUBJ\" CA cert" 1
    rm "${CREATECA_CACERT}.bak"
    $OPENSSLCMD x509 -engine qs_sig -sha256 -in $CREATECA_CACERT -noout -text
    #$OPENSSLCMD x509 -sha256 -in $CREATECA_CACERT
    # ISARA: END
}

# Create a 3rd party (existing) CA certificate
# Think of this like a common web CA as one might purchase certificates from
echo "#################################################################"
echo "####(Re)creating an initial EXTERNAL 'web root CA' certificate"
echo "#################################################################"
createCA $EST_OPENSSL_EXTCASUBJ \
    $EST_OPENSSL_EXTCADIR \
    $EST_OPENSSL_EXTCACERT \
    $EST_OPENSSL_EXTCAPRIVDIR \
    $EST_OPENSSL_EXTCASERIAL \
    $EST_OPENSSL_EXTCADATABASE \
    $EST_OPENSSL_EXTCANEWCERTSDIR \
    $EST_OPENSSL_EXTCAPRIVKEY \
    "$EST_OPENSSLCMD_EXTCANEWKEY_PARAM" \
    "$EST_OPENSSLCMD_EXTCAECPARAMSFILE" \
    $EST_OPENSSL_EXTCAPRIVKEY2 # ISARA

# Create our CA certificate
echo "#################################################################"
echo "####(Re)creating an initial root CA certificate"
echo "#################################################################"
createCA $EST_OPENSSL_CASUBJ \
    $EST_OPENSSL_CADIR \
    $EST_OPENSSL_CACERT \
    $EST_OPENSSL_CAPRIVDIR \
    $EST_OPENSSL_CASERIAL \
    $EST_OPENSSL_CADATABASE \
    $EST_OPENSSL_CANEWCERTSDIR \
    $EST_OPENSSL_CAPRIVKEY \
    "$EST_OPENSSLCMD_CANEWKEY_PARAM" \
    "$EST_OPENSSLCMD_CAECPARAMSFILE" \
    $EST_OPENSSL_CAPRIVKEY2 # ISARA

# Create a certificate for our est server
# TODO: add extension for est server
echo "#################################################################"
echo "####(Re)creating an initial peer certificate for our estServer to use"
echo "#################################################################"
# re-using the same NEWKEY_PARAM as is used for our CA
eval $OPENSSLCMD req -new -sha256 -nodes -out $EST_SERVER_CERTREQ $EST_OPENSSLCMD_CANEWKEY_PARAM -keyout $EST_SERVER_PRIVKEY -subj $EST_SERVER_SUBJ -config $EST_OPENSSL_CACNF
iferrorlogandexit "Unable to create est server CSR" 1
# ISARA: BEGIN
# generate alternative key to use as second key in multiple public-key algorithm certificate
$OPENSSLCMD genpkey -engine qs_sig -algorithm hss -pkeyopt winternitz_value:8 -pkeyopt tree_height:10 -pkeyopt aux_length:40000 -out $EST_SERVER_PRIVKEY2
iferrorlogandexit "Unable to create est server second key" 1
mv $EST_SERVER_CERTREQ "${EST_SERVER_CERTREQ}.bak"
# Convert certificate request to multiple public-key algorithm certificate request
$OPENSSLCMD reqQSExtend -engine qs_sig -reqin "${EST_SERVER_CERTREQ}.bak" -reqout $EST_SERVER_CERTREQ -privin $EST_SERVER_PRIVKEY -privqs $EST_SERVER_PRIVKEY2
iferrorlogandexit "Unable to convert to est server multiple public-key algorithm CSR" 1
rm "${EST_SERVER_CERTREQ}.bak"
# ISARA: END
$OPENSSLCMD ca -md sha256 -out $EST_SERVER_CERT -batch -config $EST_OPENSSL_CACNF -extfile ./ext.cnf -infiles $EST_SERVER_CERTREQ 
iferrorlogandexit "Unable to create est server certificate" 1
# ISARA: BEGIN
mv $EST_SERVER_CERT "${EST_SERVER_CERT}.bak"
# Convert certificate to multiple public-key algorithm certificate
$OPENSSLCMD x509QSExtend -engine qs_sig  -x509in "${EST_SERVER_CERT}.bak" -x509out $EST_SERVER_CERT -privin $EST_OPENSSL_CAPRIVKEY -reqin $EST_SERVER_CERTREQ -privqs $EST_OPENSSL_CAPRIVKEY2
iferrorlogandexit "Unable to convert to est server multiple public-key algorithm cert" 1
rm "${EST_SERVER_CERT}.bak"
$OPENSSLCMD x509 -engine qs_sig -sha256 -in $EST_SERVER_CERT -text -noout
#$OPENSSLCMD x509 -sha256 -in $EST_SERVER_CERT -text
# ISARA: END
# the mongoose https server wants to recieve the server certificate in
# a combined file:
if [ -e $EST_SERVER_CERTANDKEY ] ; then 
rm $EST_SERVER_CERTANDKEY
fi
cat $EST_SERVER_CERT > $EST_SERVER_CERTANDKEY
cat $EST_SERVER_PRIVKEY >> $EST_SERVER_CERTANDKEY

echo "#################################################################"
echo "####Creating combined trusted cert file"
cat estCA/cacert.crt extCA/cacert.crt > trustedcerts.crt

echo "#################################################################"
echo "####Setting up and launching the estServer"

# destructive creating user of "estuser" w/ password "estpwd"
# mongoose uses the same format as apache (see htpasswd)
echo "Resetting the est server password file" 
echo "estuser:estrealm:36807fa200741bb0e8fb04fcf08e2de6" > $EST_OPENSSL_CADIR/estpwdfile

# merging the two ca certs into one file so that the est server can be configured
# to use both when validating client certificates
combinefiles extCA/cacert.crt estCA/cacert.crt estCA/multicacerts.crt







