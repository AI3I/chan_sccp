Optional Cisco IP Phone XML stylesheets

These files are used only when chan_sccp is built with experimental XML support.
Copy this directory's .xsl and translations.xml files, preserving the lib/
subdirectory, to Asterisk's data directory under sccpxslt/. For example:

    /var/lib/asterisk/sccpxslt/

The module reads stylesheets from this directory and serves files from it at
its /sccpxslt HTTP endpoint. The Asterisk data directory can be changed in
asterisk.conf; use that configured location rather than assuming the example
path. These files are not installed by the module's make install target.
