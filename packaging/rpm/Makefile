PACKAGE_NAME?=	librdkafka
VERSION?=	$(shell ../get_version.py ../../src/rdkafka.h)

# Jenkins CI integration
BUILD_NUMBER?= 1

MOCK_CONFIG?=el7-x86_64.cfg

RESULT_DIR?=pkgs-$(VERSION)-$(BUILD_NUMBER)-$(MOCK_CONFIG)

all: rpm


SOURCES:
	mkdir -p SOURCES

archive: SOURCES
	cd ../../ && \
	git archive --prefix=$(PACKAGE_NAME)-$(VERSION)/ \
		-o packaging/rpm/SOURCES/$(PACKAGE_NAME)-$(VERSION).tar.gz HEAD


build_prepare: archive
	mkdir -p $(RESULT_DIR)
	rm -f $(RESULT_DIR)/$(PACKAGE_NAME)*.rpm


srpm: build_prepare
	/usr/bin/mock \
		-r $(MOCK_CONFIG) \
		--define "__version $(VERSION)" \
		--define "__release $(BUILD_NUMBER)" \
		--resultdir=$(RESULT_DIR) \
		--buildsrpm \
		--spec=librdkafka.spec \
		--sources=SOURCES
	@echo "======= Source RPM now available in $(RESULT_DIR) ======="

rpm: srpm
	/usr/bin/mock \
		-r $(MOCK_CONFIG) \
		--define "__version $(VERSION)"\
		--define "__release $(BUILD_NUMBER)"\
		--resultdir=$(RESULT_DIR) \
		--rebuild $(RESULT_DIR)/$(PACKAGE_NAME)*.src.rpm
	@echo "======= Binary RPMs now available in $(RESULT_DIR) ======="

copy-artifacts:
	cp $(RESULT_DIR)/*rpm ../../artifacts/

clean:
	rm -rf SOURCES

distclean: clean
	rm -f build.log root.log state.log available_pkgs installed_pkgs \
		*.rpm *.tar.gz

# Prepare ubuntu 14.04 for building RPMs with mock.
#  - older versions of mock needs the config file to reside in /etc/mock,
#    so we copy it there.
#  - add a mock system group (if not already exists)
#  - add the current user to the mock group.
# NOTE: This target should be run with sudo.
prepare_ubuntu:
	apt-get -qq update
#	curl -sL -o /tmp/mock.deb 'https://launchpad.net/ubuntu/+source/mock/1.3.2-1/+build/11780212/+files/mock_1.3.2-1_all.deb'
#	apt install -y /tmp/mock.deb
#	apt install -y git python-six
	apt install -y mock
	addgroup --system mock || true
	adduser $$(whoami) mock
