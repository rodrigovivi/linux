# This depends on rst2html and graphviz, and the resulting html uses
# http://wavedrom.com/ online for rendering the timeline. The offline wavedrom
# conversion seems a bit tricky to install, but is possible if needed. To edit
# the wavedrom json, copy-pasting to and from http://wavedrom.com/editor.html is
# handy as it shows the result live.

# You can set these variables from the command line.
SPHINXOPTS    =
SPHINXBUILD   = sphinx-build
PAPER         =
BUILDDIR      = _build

# Internal variables.
PAPEROPT_a4     = -D latex_paper_size=a4
PAPEROPT_letter = -D latex_paper_size=letter
ALLSPHINXOPTS   = -d $(BUILDDIR)/doctrees $(PAPEROPT_$(PAPER)) $(SPHINXOPTS) .
# the i18n builder cannot share the environment and doctrees with the others
I18NSPHINXOPTS  = $(PAPEROPT_$(PAPER)) $(SPHINXOPTS) .

.PHONY: all
all: drm-intel.html dim.html drm-misc.html

%.svg: %.dot
	dot -T svg -o $@ $<

%.html: %.rst
	rst2html $< > $@

drm-intel.html: drm-intel.rst drm-intel-flow.svg drm-intel-timeline.rst drm-intel-timeline.json

drm-misc.html: drm-misc.rst drm-misc-timeline.rst drm-misc-timeline.json drm-misc-commit-flow.svg

dim.html: dim.rst

SC_EXCLUDE := \
	-e SC2001 \
	-e SC2034 \
	-e SC2046 \
	-e SC2086 \
	-e SC2115 \
	-e SC2119 \
	-e SC2120 \
	-e SC2143

shellcheck:
	shellcheck $(SC_EXCLUDE) dim bash_completion

mancheck:
	@for cmd in $$(./dim list-commands); do \
		if ! grep -q "^$$cmd" dim.rst; then \
			echo "$@: $$cmd not documented"; \
		fi \
	done
	rst2man --strict --no-raw dim.rst >/dev/null

check: shellcheck mancheck all

.PHONY: clean
clean:
	rm -rf drm-intel.html drm-intel-flow.svg drm-misc-commit-flow.svg dim.html drm-misc.html $(BUILDDIR)

.PHONY: help
help:
	@echo "Please use \`make <target>' where <target> is one of"
	@echo "  html       to make standalone HTML files"
	@echo "  dirhtml    to make HTML files named index.html in directories"
	@echo "  singlehtml to make a single large HTML file"
	@echo "  linkcheck  to check all external links for integrity"
	@echo "  doctest    to run all doctests embedded in the documentation (if enabled)"

# FIXME: This works for the first build, but not for updates. Look into using
# Sphinx extensions for both the graphviz and wavedrom parts.
html dirhtml singlehtml linkcheck doctest: drm-intel-flow.svg drm-misc-commit-flow.svg

.PHONY: html
html:
	$(SPHINXBUILD) -b html $(ALLSPHINXOPTS) $(BUILDDIR)/html
	@echo
	@echo "Build finished. The HTML pages are in $(BUILDDIR)/html."

.PHONY: dirhtml
dirhtml:
	$(SPHINXBUILD) -b dirhtml $(ALLSPHINXOPTS) $(BUILDDIR)/dirhtml
	@echo
	@echo "Build finished. The HTML pages are in $(BUILDDIR)/dirhtml."

.PHONY: singlehtml
singlehtml:
	$(SPHINXBUILD) -b singlehtml $(ALLSPHINXOPTS) $(BUILDDIR)/singlehtml
	@echo
	@echo "Build finished. The HTML page is in $(BUILDDIR)/singlehtml."

.PHONY: linkcheck
linkcheck:
	$(SPHINXBUILD) -b linkcheck $(ALLSPHINXOPTS) $(BUILDDIR)/linkcheck
	@echo
	@echo "Link check complete; look for any errors in the above output " \
	      "or in $(BUILDDIR)/linkcheck/output.txt."

.PHONY: doctest
doctest:
	$(SPHINXBUILD) -b doctest $(ALLSPHINXOPTS) $(BUILDDIR)/doctest
	@echo "Testing of doctests in the sources finished, look at the " \
	      "results in $(BUILDDIR)/doctest/output.txt."
