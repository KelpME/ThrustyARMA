# Thrustmaster ARMA Mapper - Makefile
# Convenient commands for building, installing, and managing the mapper

.PHONY: help build install uninstall reinstall start stop restart status watch-inputs diag clean

# Default target
help:
	@echo "Thrustmaster ARMA Mapper - Available commands:"
	@echo ""
	@echo "  make build         - Build the project"
	@echo "  make install       - Install systemd service and enable auto-start"
	@echo "  make uninstall     - Remove systemd service"
	@echo "  make reinstall     - Rebuild, stop service, and restart"
	@echo ""
	@echo "  make start         - Start the mapper service"
	@echo "  make stop          - Stop the mapper service"
	@echo "  make restart       - Restart the mapper service"
	@echo "  make status        - Show service status"
	@echo ""
	@echo "  make watch-inputs  - Watch real-time axis movements (Ctrl+C to exit)"
	@echo "  make diag          - Run full diagnostics"
	@echo ""
	@echo "  make clean         - Clean build artifacts"

# Build the project
build:
	@echo "Building twcs_mapper..."
	@./build.sh build

# Install systemd service
install: build
	@echo "Installing systemd service..."
	@mkdir -p ~/.config/systemd/user
	@cp twcs-mapper.service ~/.config/systemd/user/
	@systemctl --user daemon-reload
	@systemctl --user enable twcs-mapper.service
	@echo ""
	@echo "✓ Service installed and enabled for auto-start"
	@echo "  Run 'make start' to start the service now"

# Uninstall systemd service
uninstall:
	@echo "Uninstalling systemd service..."
	@systemctl --user stop twcs-mapper.service 2>/dev/null || true
	@systemctl --user disable twcs-mapper.service 2>/dev/null || true
	@rm -f ~/.config/systemd/user/twcs-mapper.service
	@systemctl --user daemon-reload
	@echo "✓ Service uninstalled"

# Reinstall: rebuild and restart
reinstall: build stop
	@echo "Restarting mapper with new build..."
	@sleep 1
	@$(MAKE) start

# Start the mapper
start:
	@if systemctl --user is-active --quiet twcs-mapper.service; then \
		echo "Mapper is already running"; \
		$(MAKE) status; \
	else \
		echo "Starting mapper service..."; \
		systemctl --user start twcs-mapper.service; \
		sleep 1; \
		$(MAKE) status; \
	fi

# Stop the mapper
stop:
	@echo "Stopping mapper service..."
	@systemctl --user stop twcs-mapper.service 2>/dev/null || pkill -INT twcs_mapper || true
	@sleep 1
	@if pgrep -x twcs_mapper > /dev/null; then \
		echo "⚠ Mapper still running, force killing..."; \
		pkill -9 twcs_mapper; \
	else \
		echo "✓ Mapper stopped"; \
	fi

# Restart the mapper
restart: stop start

# Show service status
status:
	@echo "=== Mapper Service Status ==="
	@systemctl --user status twcs-mapper.service --no-pager 2>/dev/null || echo "Service not installed"
	@echo ""
	@echo "=== Process Status ==="
	@if pgrep -x twcs_mapper > /dev/null; then \
		echo "✓ Mapper is running (PID: $$(pgrep -x twcs_mapper))"; \
		pgrep -a twcs_mapper; \
	else \
		echo "✗ Mapper is not running"; \
	fi
	@echo ""
	@echo "=== Virtual Device Status ==="
	@if [ -e /dev/input/event20 ] && grep -q "Thrustmaster ARMA Virtual" /proc/bus/input/devices 2>/dev/null; then \
		echo "✓ Virtual controller detected:"; \
		cat /proc/bus/input/devices | grep -A5 "Thrustmaster ARMA Virtual"; \
	else \
		echo "✗ Virtual controller not found"; \
	fi

# Watch real-time axis inputs
watch-inputs:
	@echo "=== Real-time Axis Diagnostics ==="
	@echo "Move your controls to see axis mappings"
	@echo "Press Ctrl+C to exit"
	@echo ""
	@./build/bin/twcs_mapper --diag-axes

# Run full diagnostics
diag:
	@./build/bin/twcs_mapper --diagnostics

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf build/
	@echo "✓ Build directory cleaned"

# Quick run (foreground, no service)
run:
	@echo "Running mapper in foreground (Ctrl+C to stop)..."
	@./build.sh run

# Show logs
logs:
	@echo "=== Recent Mapper Logs ==="
	@journalctl --user -u twcs-mapper.service -n 50 --no-pager
