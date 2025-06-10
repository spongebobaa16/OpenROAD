#!/usr/bin/env python3
"""
DEF ECO Analyzer
Compares original and modified DEF files to generate ECO changelist
including sizing and buffering commands.
"""

"""
usage: python def_eco_analyzer.py <original_def> <modified_def> [output_file]

Example: python def_eco_analyzer.py aes_cipher_top_orig.def aes_cipher_top_repair2.def eco_changes.txt
"""


import re
import sys
from typing import Dict, Set, List, Tuple
from collections import defaultdict

class DEFParser:
    def __init__(self, def_file_path: str):
        self.def_file_path = def_file_path
        self.components = {}  # {cell_name: cell_type}
        self.nets = {}  # {net_name: [(instance, pin), ...]}
        self.parse_def()
    
    def parse_def(self):
        """Parse DEF file to extract components and nets"""
        with open(self.def_file_path, 'r') as f:
            content = f.read()
        
        # Parse components section
        self._parse_components(content)
        
        # Parse nets section
        self._parse_nets(content)
    
    def _parse_components(self, content: str):
        """Parse COMPONENTS section"""
        # Find COMPONENTS section
        components_match = re.search(r'COMPONENTS\s+(\d+)\s*;(.*?)END\s+COMPONENTS', content, re.DOTALL)
        if not components_match:
            print("Warning: No COMPONENTS section found")
            return
        
        components_content = components_match.group(2)
        
        # Parse individual components
        # Pattern: - <instance_name> <cell_type> + ... ;
        component_pattern = r'-\s+(\S+)\s+(\S+)\s+[^;]*;'
        matches = re.findall(component_pattern, components_content)
        
        for instance_name, cell_type in matches:
            self.components[instance_name] = cell_type
    
    def _parse_nets(self, content: str):
        """Parse NETS section"""
        # Find NETS section
        nets_match = re.search(r'NETS\s+(\d+)\s*;(.*?)END\s+NETS', content, re.DOTALL)
        if not nets_match:
            print("Warning: No NETS section found")
            return
        
        nets_content = nets_match.group(2)
        
        # Parse individual nets - handle multi-line definitions
        # Split by lines starting with "- " but preserve multi-line entries until ";"
        net_entries = []
        current_entry = ""
        
        for line in nets_content.split('\n'):
            line = line.strip()
            if not line:
                continue
            
            if line.startswith('- '):
                # Start of new net entry
                if current_entry:
                    net_entries.append(current_entry)
                current_entry = line[2:]  # Remove "- " prefix
            elif current_entry:
                # Continuation of current net entry
                current_entry += " " + line
            
            # Check if entry is complete (ends with semicolon)
            if current_entry and current_entry.rstrip().endswith(';'):
                net_entries.append(current_entry)
                current_entry = ""
        
        # Add any remaining entry
        if current_entry:
            net_entries.append(current_entry)
        
        # Parse each complete net entry
        for net_entry in net_entries:
            if not net_entry.strip():
                continue
            
            # Remove trailing semicolon and any extra whitespace
            net_entry = net_entry.rstrip(';').strip()
            
            # Extract net name and connections
            # Format: netname ( instance pin ) ( instance pin ) ... + USE SIGNAL
            parts = net_entry.split('(', 1)
            if len(parts) < 2:
                continue
            
            net_name = parts[0].strip()
            connections_str = '(' + parts[1]
            
            # Parse connections: ( instance pin )
            connections = []
            conn_pattern = r'\(\s*(\S+)\s+(\S+)\s*\)'
            conn_matches = re.findall(conn_pattern, connections_str)
            
            for instance, pin in conn_matches:
                connections.append((instance, pin))
            
            if connections:  # Only store nets with connections
                self.nets[net_name] = connections

class ECOAnalyzer:
    def __init__(self, original_def: str, modified_def: str):
        self.original = DEFParser(original_def)
        self.modified = DEFParser(modified_def)
        self.sizing_commands = []
        self.buffering_commands = []
    
    def analyze(self):
        """Analyze differences and generate ECO commands"""
        self._find_sizing_changes()
        self._find_buffering_changes()
    
    def _find_sizing_changes(self):
        """Find cells that have been resized"""
        print("Analyzing sizing changes...")
        
        # Find common cells that changed type
        common_cells = set(self.original.components.keys()) & set(self.modified.components.keys())
        
        for cell_name in common_cells:
            orig_type = self.original.components[cell_name]
            mod_type = self.modified.components[cell_name]
            
            if orig_type != mod_type:
                # Check if it's the same function (e.g., INVx3 -> INVx8)
                if self._is_same_function(orig_type, mod_type):
                    self.sizing_commands.append(f"size_cell {cell_name} {mod_type}")
    
    def _is_same_function(self, orig_type: str, mod_type: str) -> bool:
        """Check if two cell types have the same function but different sizes"""
        # Extract base function (remove size/strength indicators)
        def get_base_function(cell_type):
            # Remove common size indicators like x1, x2, x3, x4, x6, x8, xp5, etc.
            # Handle both integer (x1, x2) and fractional (xp5 = x0.5) drive strengths
            base = re.sub(r'x(?:\d+|p\d+)[a-zA-Z]*', 'x', cell_type)
            return base
        
        return get_base_function(orig_type) == get_base_function(mod_type)
    
    def _find_buffering_changes(self):
        """Find buffer insertions"""
        print("Analyzing buffering changes...")
        
        # Find new components in modified design
        new_components = set(self.modified.components.keys()) - set(self.original.components.keys())
        
        # Store all buffer commands before sorting
        buffer_commands = []
        
        for new_comp in new_components:
            comp_type = self.modified.components[new_comp]
            
            # Check if it's a buffer (contains BUF or HB1 or HB2)
            if 'BUF' in comp_type or 'HB1' in comp_type or 'HB2' in comp_type:
                cmd = self._analyze_buffer_insertion(new_comp, comp_type)
                if cmd:  # Only add if command was generated
                    buffer_commands.append(cmd)
        
        # Sort commands based on dependencies
        self.buffering_commands = self._sort_buffer_commands(buffer_commands)
    
    def _analyze_buffer_insertion(self, buffer_name: str, buffer_type: str):
        """Analyze a specific buffer insertion and return command string"""
        # Find the net driven by this buffer (buffer output)
        buffer_output_net = None
        buffer_input_net = None
        
        for net_name, connections in self.modified.nets.items():
            for instance, pin in connections:
                if instance == buffer_name:
                    if pin in ['Y', 'Z', 'Q']:  # Common output pin names
                        buffer_output_net = net_name
                    elif pin in ['A', 'D', 'IN']:  # Common input pin names
                        buffer_input_net = net_name
        
        if not buffer_output_net:
            print(f"Warning: Could not find output net for buffer {buffer_name}")
            return None
        
        # Find load pins driven by this buffer
        load_pins = []
        if buffer_output_net in self.modified.nets:
            for instance, pin in self.modified.nets[buffer_output_net]:
                if instance != buffer_name:  # Exclude the buffer itself
                    load_pins.append(f"{instance}/{pin}")
        
        if load_pins:
            load_pins_str = "{" + " ".join(load_pins) + "}"
            buffered_net_name = buffer_output_net
            
            cmd = f"insert_buffer {load_pins_str} {buffer_type} {buffer_name} {buffered_net_name}"
            return cmd
        
        return None
    
    def _sort_buffer_commands(self, buffer_commands):
        """Sort buffer commands to resolve dependencies"""
        print("Sorting buffer commands by dependencies...")
        
        if not buffer_commands:
            return []
            
        # Build dependency graph
        buffer_info = {}  # {buffer_name: (command, dependencies)}
        all_buffer_names = set()
        
        # First pass: collect all buffer names and their basic info
        for cmd in buffer_commands:
            parts = cmd.split()
            if len(parts) >= 4:
                buffer_name = parts[-2]  # new buffer cell name
                all_buffer_names.add(buffer_name)
                buffer_info[buffer_name] = (cmd, set())
        
        # Second pass: analyze dependencies
        for cmd in buffer_commands:
            parts = cmd.split()
            if len(parts) >= 4:
                buffer_name = parts[-2]  # new buffer cell name
                dependencies = set()
                
                # Extract load pins from the command
                load_pins_str = cmd.split('{')[1].split('}')[0]
                load_pins = [pin.strip() for pin in load_pins_str.split()]
                
                # Check if any load pin belongs to another buffer
                for load_pin in load_pins:
                    if '/' in load_pin:
                        instance = load_pin.split('/')[0]
                        # If this instance is another buffer that needs to be created
                        if instance in all_buffer_names and instance != buffer_name:
                            dependencies.add(instance)
                
                # Additionally, check if this buffer's output net is used by other buffers
                # If buffer A creates net X, and buffer B uses net X as input, then A must come before B
                buffered_net = parts[-1]  # new buffered net name
                
                # Check if any other buffer uses this net as input
                for other_cmd in buffer_commands:
                    if other_cmd == cmd:
                        continue
                    other_parts = other_cmd.split()
                    if len(other_parts) >= 4:
                        other_buffer_name = other_parts[-2]
                        # Check if this buffered net appears in other buffer's load pins
                        if buffered_net in other_cmd and other_buffer_name != buffer_name:
                            # This means other_buffer depends on current buffer
                            # So we don't add dependency here, but we'll handle it in the reverse
                            pass
                
                buffer_info[buffer_name] = (cmd, dependencies)
        
        # Third pass: Add reverse dependencies
        # If buffer A's output is used by buffer B, then A must come before B
        for cmd in buffer_commands:
            parts = cmd.split()
            if len(parts) >= 4:
                buffer_name = parts[-2]
                buffered_net = parts[-1]
                
                # Find other buffers that use this net
                for other_cmd in buffer_commands:
                    if other_cmd == cmd:
                        continue
                    other_parts = other_cmd.split()
                    if len(other_parts) >= 4:
                        other_buffer_name = other_parts[-2]
                        
                        # Check if the buffered net appears as input to other buffer
                        other_load_pins_str = other_cmd.split('{')[1].split('}')[0]
                        other_load_pins = [pin.strip() for pin in other_load_pins_str.split()]
                        
                        for load_pin in other_load_pins:
                            # If the load pin references the buffered net or buffer
                            if ('split' in load_pin and buffered_net.replace('net', 'split') in load_pin) or \
                               (buffer_name in load_pin):
                                # other_buffer depends on current buffer
                                _, other_deps = buffer_info[other_buffer_name]
                                other_deps.add(buffer_name)
                                buffer_info[other_buffer_name] = (buffer_info[other_buffer_name][0], other_deps)
        
        # Topological sort to resolve dependencies
        sorted_commands = []
        remaining = dict(buffer_info)
        created_buffers = set()
        
        iteration = 0
        while remaining:
            iteration += 1
            if iteration > 1000:  # Prevent infinite loop
                print(f"Warning: Breaking infinite loop, remaining buffers: {len(remaining)}")
                break
                
            # Find buffers with no unsatisfied dependencies
            ready_buffers = []
            for buf_name, (cmd, deps) in remaining.items():
                if deps.issubset(created_buffers):
                    ready_buffers.append(buf_name)
            
            if not ready_buffers:
                # Circular dependency or error - add remaining in original order
                print(f"Warning: Possible circular dependency in buffer commands, remaining: {len(remaining)}")
                for buf_name, (cmd, deps) in remaining.items():
                    print(f"  {buf_name}: deps={deps}")
                    sorted_commands.append(cmd)
                break
            
            # Sort ready buffers by name for consistent output
            ready_buffers.sort()
            
            # Add ready buffers to sorted list
            for buf_name in ready_buffers:
                sorted_commands.append(remaining[buf_name][0])
                created_buffers.add(buf_name)
                del remaining[buf_name]
        
        print(f"Sorted {len(sorted_commands)} buffer commands")
        return sorted_commands
    
    def print_results(self):
        """Print the ECO changelist"""
        print("\n" + "="*80)
        print("ECO CHANGELIST")
        print("="*80)
        
        print(f"\nSizing Commands ({len(self.sizing_commands)}):")
        print("-" * 40)
        if self.sizing_commands:
            for i, cmd in enumerate(self.sizing_commands, 1):
                print(f"{i:2d}. {cmd}")
        else:
            print("   No sizing changes found.")
        
        print(f"\nBuffering Commands ({len(self.buffering_commands)}):")
        print("-" * 40)
        if self.buffering_commands:
            for i, cmd in enumerate(self.buffering_commands, 1):
                print(f"{i:2d}. {cmd}")
        else:
            print("   No buffer insertions found.")
        
        print(f"\nTotal ECO Changes: {len(self.sizing_commands) + len(self.buffering_commands)}")
    
    def save_results(self, output_file: str):
        """Save results to file"""
        with open(output_file, 'w') as f:
            f.write("ECO CHANGELIST\n")
            f.write("="*80 + "\n\n")
            
            f.write(f"Sizing Commands ({len(self.sizing_commands)}):\n")
            f.write("-" * 40 + "\n")
            if self.sizing_commands:
                for i, cmd in enumerate(self.sizing_commands, 1):
                    f.write(f"{i:2d}. {cmd}\n")
            else:
                f.write("   No sizing changes found.\n")
            
            f.write(f"\nBuffering Commands ({len(self.buffering_commands)}):\n")
            f.write("-" * 40 + "\n")
            if self.buffering_commands:
                for i, cmd in enumerate(self.buffering_commands, 1):
                    f.write(f"{i:2d}. {cmd}\n")
            else:
                f.write("   No buffer insertions found.\n")
            
            f.write(f"\nTotal ECO Changes: {len(self.sizing_commands) + len(self.buffering_commands)}\n")

def main():
    if len(sys.argv) < 3:
        print("Usage: python def_eco_analyzer.py <original_def> <modified_def> [output_file]")
        print("Example: python def_eco_analyzer.py aes_cipher_top_orig.def aes_cipher_top_repair2.def eco_changes.txt")
        sys.exit(1)
    
    original_def = sys.argv[1]
    modified_def = sys.argv[2]
    output_file = sys.argv[3] if len(sys.argv) > 3 else None
    
    try:
        print(f"Analyzing DEF files:")
        print(f"  Original: {original_def}")
        print(f"  Modified: {modified_def}")
        
        analyzer = ECOAnalyzer(original_def, modified_def)
        analyzer.analyze()
        analyzer.print_results()
        
        if output_file:
            analyzer.save_results(output_file)
            print(f"\nResults saved to: {output_file}")
    
    except FileNotFoundError as e:
        print(f"Error: File not found - {e}")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main() 