# Makefile raiz: orquesta la compilacion del editor standalone (editor/) y
# del shell educativo que lo integra (shell/). Ver cada subcarpeta para el
# detalle de flags de compilacion.

all:
	$(MAKE) -C editor all
	$(MAKE) -C shell all
	@echo ""
	@echo "Listo. Binarios generados:"
	@echo "  editor/editor   (standalone: ./editor/editor [archivo])"
	@echo "  shell/eafitOS   (shell integrador: comando 'editor [archivo]')"

clean:
	$(MAKE) -C editor clean
	$(MAKE) -C shell clean

.PHONY: all clean
